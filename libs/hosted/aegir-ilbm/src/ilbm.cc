/*
 * ILBM decode -- implementation. See include/aegir/ilbm.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * IFF is big-endian and so is every field here, while the machine is not: the
 * readers spell the byte order out rather than casting. A chunk's data is
 * padded to an even length, and an ILBM row is word-aligned *per plane*, which
 * is the arithmetic that goes wrong quietly -- a row one byte short shifts
 * every row after it and draws a smudge.
 */

#include <aegir/ilbm.h>

#include <aegir/datatypes.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace aegir::datatypes::ilbm {
namespace {

uint16_t be16(const uint8_t *p) noexcept
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t be32(const uint8_t *p) noexcept
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool id_is(const uint8_t *p, const char *id) noexcept
{
    return p[0] == static_cast<uint8_t>(id[0]) && p[1] == static_cast<uint8_t>(id[1]) &&
           p[2] == static_cast<uint8_t>(id[2]) && p[3] == static_cast<uint8_t>(id[3]);
}

struct Chunks {
    const uint8_t *bmhd = nullptr;
    size_t bmhd_size = 0;
    const uint8_t *cmap = nullptr;
    size_t cmap_size = 0;
    const uint8_t *body = nullptr;
    size_t body_size = 0;
};

/* Walk the FORM's chunks, keeping the three this reader needs and ignoring the
 * rest (CAMG, CRNG, ANNO, DEST, ...). A chunk whose data runs past the FORM is
 * a malformed file, not something to read past. */
bool scan(const uint8_t *data, size_t size, Chunks &out) noexcept
{
    if (size < 12 || !id_is(data, "FORM") || !id_is(data + 8, "ILBM")) return false;
    const uint32_t form = be32(data + 4);
    if (static_cast<size_t>(8) + form > size) return false;
    const size_t end = 8 + form;
    size_t p = 12;
    while (p + 8 <= end) {
        const uint8_t *const id = data + p;
        const uint32_t n = be32(data + p + 4);
        const size_t body = p + 8;
        if (body + n > end) return false;
        if (id_is(id, "BMHD")) {
            out.bmhd = data + body;
            out.bmhd_size = n;
        } else if (id_is(id, "CMAP")) {
            out.cmap = data + body;
            out.cmap_size = n;
        } else if (id_is(id, "BODY")) {
            out.body = data + body;
            out.body_size = n;
        }
        p = body + n + (n & 1u);
    }
    return true;
}

struct Header {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t planes = 0;
    uint8_t masking = 0; /* 0 none, 1 a mask plane, 2 a transparent colour */
    uint8_t compression = 0;
    uint16_t transparent = 0;
    uint32_t palette_size = 0;
};

bool parse_header(const Chunks &c, Header &h) noexcept
{
    if (c.bmhd == nullptr || c.bmhd_size < 20 || c.body == nullptr) return false;
    h.width = be16(c.bmhd + 0);
    h.height = be16(c.bmhd + 2);
    h.planes = c.bmhd[8];
    h.masking = c.bmhd[9];
    h.compression = c.bmhd[10];
    h.transparent = be16(c.bmhd + 12);
    if (h.width == 0 || h.height == 0) return false;
    if (h.planes < 1 || h.planes > 8) return false;
    if (h.masking > 2 || h.compression > 1) return false;
    if (c.cmap != nullptr) {
        h.palette_size = static_cast<uint32_t>(c.cmap_size / 3);
        if (h.palette_size > 256) h.palette_size = 256;
    } else {
        h.palette_size = 1u << h.planes;
    }
    return true;
}

/* IFF ByteRun1 (the PackBits shape): a non-negative count copies count+1
 * literals, a negative one repeats the next byte 1-count times, and -128 is a
 * no-op. Runs may cross a row's or a plane's boundary, so the whole BODY is
 * unpacked before it is indexed. */
bool unpack(const uint8_t *src, size_t src_size, size_t need,
            std::vector<uint8_t> &out) noexcept
{
    out.clear();
    out.reserve(need);
    size_t i = 0;
    while (out.size() < need && i < src_size) {
        const int8_t n = static_cast<int8_t>(src[i++]);
        if (n >= 0) {
            const size_t count = static_cast<size_t>(n) + 1;
            if (i + count > src_size) return false;
            out.insert(out.end(), src + i, src + i + count);
            i += count;
        } else if (n != -128) {
            if (i >= src_size) return false;
            const size_t count = static_cast<size_t>(1 - n);
            out.insert(out.end(), count, src[i++]);
        }
    }
    return out.size() >= need;
}

} // namespace

bool identify(const uint8_t *data, size_t size) noexcept
{
    return size >= 12 && id_is(data, "FORM") && id_is(data + 8, "ILBM");
}

bool probe(const uint8_t *data, size_t size, Info &out) noexcept
{
    Chunks c;
    Header h;
    if (!scan(data, size, c) || !parse_header(c, h)) return false;
    out.width = h.width;
    out.height = h.height;
    out.format = Format::INDEXED;
    out.stride = h.width; /* one byte per pixel */
    out.palette_size = h.palette_size;
    out.transparent = h.masking == 2;
    out.transparent_index = h.masking == 2 ? h.transparent : 0;
    return true;
}

bool decode(const uint8_t *data, size_t size, Bitmap &bitmap) noexcept
{
    Chunks c;
    Header h;
    if (!scan(data, size, c) || !parse_header(c, h)) return false;

    const size_t row_bytes = ((static_cast<size_t>(h.width) + 15) / 16) * 2;
    const size_t planes = static_cast<size_t>(h.planes) + (h.masking == 1 ? 1 : 0);
    const size_t need = row_bytes * planes * h.height;
    const size_t stride = h.width;
    if (bitmap.pixels == nullptr || bitmap.pixels_size < stride * h.height) return false;
    if (h.palette_size != 0 &&
        (bitmap.palette == nullptr || bitmap.palette_size < h.palette_size)) {
        return false;
    }

    const uint8_t *rows = c.body;
    std::vector<uint8_t> unpacked;
    if (h.compression == 1) {
        if (!unpack(c.body, c.body_size, need, unpacked)) return false;
        rows = unpacked.data();
    } else if (c.body_size < need) {
        return false;
    }

    std::memset(bitmap.pixels, 0, stride * h.height);
    for (uint32_t y = 0; y < h.height; ++y) {
        for (uint8_t p = 0; p < h.planes; ++p) {
            const uint8_t *const plane =
                rows + (static_cast<size_t>(y) * planes + p) * row_bytes;
            for (uint32_t x = 0; x < h.width; ++x) {
                const uint8_t bit = (plane[x >> 3] >> (7 - (x & 7))) & 1u;
                bitmap.pixels[static_cast<size_t>(y) * stride + x] |=
                    static_cast<uint8_t>(bit << p);
            }
        }
    }

    for (uint32_t i = 0; i < h.palette_size; ++i) {
        if (c.cmap != nullptr) {
            bitmap.palette[i] = Color{c.cmap[i * 3], c.cmap[i * 3 + 1], c.cmap[i * 3 + 2]};
        } else {
            const uint8_t v = h.palette_size <= 2
                                  ? static_cast<uint8_t>(i * 255)
                                  : static_cast<uint8_t>((i * 255) / (h.palette_size - 1));
            bitmap.palette[i] = Color{v, v, v};
        }
    }
    return true;
}

} // namespace aegir::datatypes::ilbm
