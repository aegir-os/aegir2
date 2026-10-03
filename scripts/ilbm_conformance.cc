/*
 * Host conformance for the ILBM decoder (specs/datatypes.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * scripts/check_ilbm.py compiles this with the host compiler against ilbm.cc
 * and runs it; it is not part of any target build. The images are built here
 * as IFF bytes -- BMHD, CMAP and BODY -- so the tests are pure: no filesystem,
 * no service, no vendored tree. What they pin is the arithmetic that goes
 * wrong quietly -- the big-endian fields, the per-plane word-aligned row, the
 * ByteRun1 runs, the palette -- and the frame stream the class serves a page
 * at a time.
 */

#include <aegir/datatypes.h>
#include <aegir/datatype/decoded.h>
#include <aegir/ilbm.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using aegir::datatypes::Color;
using aegir::datatypes::Decoded;
using aegir::datatypes::Format;
using aegir::datatypes::Info;

int g_checks = 0;
int g_failures = 0;

void expect(bool condition, char const *what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL  %s\n", what);
    }
}

using Bytes = std::vector<uint8_t>;

void put16(Bytes &b, uint16_t v)
{
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v & 0xff));
}

void put32(Bytes &b, uint32_t v)
{
    b.push_back(static_cast<uint8_t>(v >> 24));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>(v & 0xff));
}

void put_id(Bytes &b, char const *id)
{
    b.insert(b.end(), id, id + 4);
}

void put_chunk(Bytes &b, char const *id, Bytes const &data)
{
    put_id(b, id);
    put32(b, static_cast<uint32_t>(data.size()));
    b.insert(b.end(), data.begin(), data.end());
    if (data.size() & 1u) b.push_back(0);
}

Bytes make_form(Bytes const &chunks)
{
    Bytes b;
    put_id(b, "FORM");
    put32(b, static_cast<uint32_t>(chunks.size()) + 4);
    put_id(b, "ILBM");
    b.insert(b.end(), chunks.begin(), chunks.end());
    return b;
}

Bytes bmhd(uint16_t w, uint16_t h, uint8_t planes, uint8_t masking,
           uint8_t compression, uint16_t transparent)
{
    Bytes d;
    put16(d, w);
    put16(d, h);
    put16(d, 0); /* x */
    put16(d, 0); /* y */
    d.push_back(planes);
    d.push_back(masking);
    d.push_back(compression);
    d.push_back(0);
    put16(d, transparent);
    d.push_back(1); /* xAspect */
    d.push_back(1); /* yAspect */
    put16(d, w);    /* pageWidth */
    put16(d, h);    /* pageHeight */
    return d;
}

Bytes cmap_of(std::vector<Color> const &colors)
{
    Bytes d;
    for (Color const &c : colors) {
        d.push_back(c.r);
        d.push_back(c.g);
        d.push_back(c.b);
    }
    return d;
}

/* The row-major, plane-major BODY for per-pixel indices. The inverse of what
 * the decoder does, so a case states the picture and not the bits. */
Bytes planes_of(uint16_t w, uint16_t h, uint8_t planes, std::vector<uint8_t> const &idx)
{
    const size_t row = ((static_cast<size_t>(w) + 15) / 16) * 2;
    Bytes body(row * planes * h, 0);
    for (uint16_t y = 0; y < h; ++y) {
        for (uint8_t p = 0; p < planes; ++p) {
            uint8_t *const plane =
                body.data() + (static_cast<size_t>(y) * planes + p) * row;
            for (uint16_t x = 0; x < w; ++x) {
                const uint8_t bit = (idx[static_cast<size_t>(y) * w + x] >> p) & 1u;
                plane[x >> 3] |= static_cast<uint8_t>(bit << (7 - (x & 7)));
            }
        }
    }
    return body;
}

/* All-literal ByteRun1, in runs of at most 128 -- the simple path. */
Bytes pack_literal(Bytes const &src)
{
    Bytes out;
    size_t i = 0;
    while (i < src.size()) {
        size_t n = src.size() - i;
        if (n > 128) n = 128;
        out.push_back(static_cast<uint8_t>(n - 1));
        out.insert(out.end(), src.begin() + static_cast<long>(i),
                   src.begin() + static_cast<long>(i + n));
        i += n;
    }
    return out;
}

Bytes ilbm(uint16_t w, uint16_t h, uint8_t planes, uint8_t masking,
           uint8_t compression, uint16_t transparent, Bytes const &palette,
           Bytes const &body)
{
    Bytes chunks;
    put_chunk(chunks, "BMHD", bmhd(w, h, planes, masking, compression, transparent));
    if (!palette.empty()) put_chunk(chunks, "CMAP", palette);
    put_chunk(chunks, "BODY", body);
    return make_form(chunks);
}

void check_identify()
{
    Bytes const good = ilbm(1, 1, 1, 0, 0, 0, cmap_of({{0, 0, 0}, {255, 255, 255}}),
                            planes_of(1, 1, 1, {1}));
    expect(aegir::datatypes::ilbm::identify(good.data(), good.size()),
           "FORM/ILBM identifies");
    Bytes const junk = {'n', 'o', 'p', 'e', 0, 0, 0, 0, 0, 0, 0, 0};
    expect(!aegir::datatypes::ilbm::identify(junk.data(), junk.size()),
           "garbage does not identify");
}

void check_uncompressed()
{
    constexpr uint16_t w = 4;
    constexpr uint16_t h = 2;
    std::vector<uint8_t> const idx = {0, 1, 2, 3, 3, 2, 1, 0};
    std::vector<Color> const pal = {{0, 0, 0}, {1, 2, 3}, {255, 0, 0}, {0, 0, 255}};
    Bytes const file = ilbm(w, h, 2, 0, 0, 0, cmap_of(pal), planes_of(w, h, 2, idx));

    Info info{};
    expect(aegir::datatypes::ilbm::probe(file.data(), file.size(), info),
           "probe 4x2");
    expect(info.width == 4 && info.height == 2 && info.format == Format::INDEXED &&
               info.stride == 4 && info.palette_size == 4,
           "probe states size, layout, stride and palette");

    Decoded d;
    expect(aegir::datatypes::ilbm::decode(file.data(), file.size(), d), "decode 4x2");
    expect(d.pixels == idx, "decode lands the pixels");
    expect(d.palette[1].r == 1 && d.palette[1].g == 2 && d.palette[1].b == 3,
           "decode copies the palette");

    /* The stream is the pixels then the palette, and the class serves it by
     * offset -- three bytes at a time here, as a caller would a page at a
     * time. */
    std::size_t const total = d.stream_size();
    expect(total == idx.size() + pal.size() * 3, "the stream is pixels then palette");
    Bytes whole(total, 0);
    expect(d.read(0, whole.data(), whole.size()) == total, "read the whole frame");
    bool stream_ok = true;
    for (std::size_t i = 0; i < idx.size(); ++i) {
        if (whole[i] != idx[i]) stream_ok = false;
    }
    for (std::size_t i = 0; i < pal.size(); ++i) {
        if (whole[idx.size() + i * 3] != pal[i].r ||
            whole[idx.size() + i * 3 + 1] != pal[i].g ||
            whole[idx.size() + i * 3 + 2] != pal[i].b) {
            stream_ok = false;
        }
    }
    expect(stream_ok, "the stream's bytes are the pixels then the palette");

    Bytes paged(total, 0);
    std::size_t at = 0;
    while (at < total) {
        const std::size_t n = d.read(at, paged.data() + at, 3);
        if (n == 0) break;
        at += n;
    }
    expect(at == total && paged == whole, "a three-byte-at-a-time read assembles it");
    expect(d.read(total, paged.data(), 3) == 0, "a read past the end is empty");
}

void check_compressed()
{
    constexpr uint16_t w = 4;
    constexpr uint16_t h = 2;
    std::vector<uint8_t> const idx = {0, 1, 2, 3, 3, 2, 1, 0};
    std::vector<Color> const pal = {{0, 0, 0}, {1, 2, 3}, {255, 0, 0}, {0, 0, 255}};
    Bytes const raw = planes_of(w, h, 2, idx);
    Bytes const file = ilbm(w, h, 2, 0, 1, 0, cmap_of(pal), pack_literal(raw));

    Decoded d;
    expect(aegir::datatypes::ilbm::decode(file.data(), file.size(), d),
           "decode 4x2 ByteRun1 literals");
    expect(d.pixels == idx, "ByteRun1 literals land the same pixels");
}

void check_repeat_run()
{
    /* Eight pixels, one plane, all index 0: two row bytes, encoded as one
     * repeat run of two. 0xFF is -1, so the byte after repeats twice. */
    Bytes const body = {0xFF, 0x00};
    Bytes const file = ilbm(8, 1, 1, 0, 1, 0, cmap_of({{0, 0, 0}, {255, 255, 255}}),
                            body);
    Decoded d;
    expect(aegir::datatypes::ilbm::decode(file.data(), file.size(), d),
           "decode a ByteRun1 repeat");
    expect(d.pixels == std::vector<uint8_t>(8, 0), "a repeat run lands its bytes");
}

void check_width_not_word()
{
    /* Width 5 is not a multiple of 16: a plane row is two bytes, and only the
     * first byte's high five bits carry pixels. A row one byte short is the
     * classic smudge. */
    constexpr uint16_t w = 5;
    std::vector<uint8_t> const idx = {1, 0, 1, 0, 1};
    Bytes const file = ilbm(w, 1, 1, 0, 0, 0, cmap_of({{0, 0, 0}, {255, 255, 255}}),
                            planes_of(w, 1, 1, idx));
    Info info{};
    expect(aegir::datatypes::ilbm::probe(file.data(), file.size(), info),
           "probe width 5");
    expect(info.stride == 5, "width 5 strides 5, not the padded row");
    Decoded d;
    expect(aegir::datatypes::ilbm::decode(file.data(), file.size(), d),
           "decode width 5");
    expect(d.pixels == idx, "width 5 lands across the first byte");
}

void check_transparent()
{
    Bytes const file = ilbm(2, 1, 1, 2, 0, 1, cmap_of({{0, 0, 0}, {255, 0, 0}}),
                            planes_of(2, 1, 1, {0, 1}));
    Info info{};
    expect(aegir::datatypes::ilbm::probe(file.data(), file.size(), info),
           "probe a transparent colour");
    expect(info.transparent && info.transparent_index == 1,
           "masking 2 states the transparent index");
}

void check_no_cmap()
{
    Bytes const file = ilbm(2, 1, 3, 0, 0, 0, {}, planes_of(2, 1, 3, {0, 7}));
    Info info{};
    expect(aegir::datatypes::ilbm::probe(file.data(), file.size(), info),
           "probe without a CMAP");
    expect(info.palette_size == 8, "no CMAP yields 2^planes palette entries");
    Decoded d;
    expect(aegir::datatypes::ilbm::decode(file.data(), file.size(), d),
           "decode without a CMAP");
    expect(d.palette[7].r == 255 && d.palette[7].g == 255 && d.palette[7].b == 255,
           "the default palette is a ramp to white");
}

void check_malformed()
{
    Bytes const good = ilbm(2, 1, 1, 0, 0, 0, cmap_of({{0, 0, 0}, {255, 0, 0}}),
                            planes_of(2, 1, 1, {0, 1}));
    Info info{};
    expect(!aegir::datatypes::ilbm::probe(good.data(), good.size() - 1, info),
           "a file one byte short does not probe");
    Decoded d;
    expect(!aegir::datatypes::ilbm::decode(good.data(), good.size() - 1, d),
           "a file one byte short does not decode");

    /* A BODY that does not hold the rows BMHD promised is refused. */
    Bytes const chunks_short = [] {
        Bytes chunks;
        put_chunk(chunks, "BMHD", bmhd(4, 4, 1, 0, 0, 0));
        put_chunk(chunks, "BODY", Bytes{0x00, 0x00});
        return make_form(chunks);
    }();
    Decoded e;
    expect(!aegir::datatypes::ilbm::decode(chunks_short.data(), chunks_short.size(), e),
           "a BODY too short for BMHD is refused");
}

} // namespace

int main()
{
    check_identify();
    check_uncompressed();
    check_compressed();
    check_repeat_run();
    check_width_not_word();
    check_transparent();
    check_no_cmap();
    check_malformed();

    std::printf("ILBM: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
