/*
 * Host conformance for the PNG decoder (specs/datatypes.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * scripts/check_png.py builds the vendored zlib and libpng for the host,
 * compiles this with the host compiler against png.cc, and runs it; it is not
 * part of any target build. The PNGs are built here as bytes -- signature,
 * IHDR, PLTE, an IDAT of zlib-compressed scanlines, IEND -- so the tests are
 * pure: no filesystem, no service, no encoder. What they pin is the
 * normalisation our decoder states and the frame it serves: every colour type
 * arrives as the one RGBA layout, the stride is width*4, no palette survives,
 * and the whole image is in row order -- the parts of the contract that go
 * wrong quietly and would show as a smudge in the demo.
 */

#include <aegir/datatypes.h>
#include <aegir/datatype/decoded.h>
#include <aegir/png.h>

#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

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

void put32(Bytes &out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(v & 0xff));
}

/* A PNG chunk: length, type, data, and the CRC over type+data (not length). */
void put_chunk(Bytes &out, char const *id, Bytes const &data)
{
    put32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), id, id + 4);
    out.insert(out.end(), data.begin(), data.end());
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<Bytef const *>(id), 4);
    if (!data.empty()) {
        crc = crc32(crc, data.data(), static_cast<uInt>(data.size()));
    }
    put32(out, static_cast<uint32_t>(crc));
}

/* A whole PNG from raw scanlines (each row already carries its filter byte).
 * `plte` is the optional palette for colour type 3. */
Bytes png_bytes(uint32_t width, uint32_t height, uint8_t colour_type, uint8_t bit_depth,
                Bytes const &raw, Bytes const &plte = {})
{
    Bytes out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    Bytes ihdr;
    put32(ihdr, width);
    put32(ihdr, height);
    ihdr.push_back(bit_depth);
    ihdr.push_back(colour_type);
    ihdr.push_back(0); /* compression: deflate */
    ihdr.push_back(0); /* filter method */
    ihdr.push_back(0); /* interlace: none */
    put_chunk(out, "IHDR", ihdr);
    if (!plte.empty()) {
        put_chunk(out, "PLTE", plte);
    }
    uLongf bound = compressBound(static_cast<uLong>(raw.size()));
    Bytes z(static_cast<std::size_t>(bound));
    uLongf zlen = bound;
    compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 6);
    z.resize(static_cast<std::size_t>(zlen));
    put_chunk(out, "IDAT", z);
    put_chunk(out, "IEND", {});
    return out;
}

/* Raw scanlines for an 8-bit RGBA image from a flat pixel list (one filter
 * byte, 0, per row). */
Bytes rgba_rows(uint32_t width, uint32_t height, Bytes const &pixels)
{
    Bytes raw;
    raw.reserve(static_cast<std::size_t>(height) * (1 + width * 4));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), pixels.begin() + static_cast<std::ptrdiff_t>(y) * width * 4,
                   pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * width * 4));
    }
    return raw;
}

void check_identify()
{
    Bytes const file = png_bytes(1, 1, 6, 8, {0, 0, 0, 0, 0});
    expect(aegir::datatypes::png::identify(file.data(), file.size()),
           "a real PNG identifies");
    expect(!aegir::datatypes::png::identify(file.data(), 4),
           "four bytes are too short to be a PNG");
    expect(!aegir::datatypes::png::identify(nullptr, 0),
           "a null pointer identifies nothing");

    Bytes garbage = {0x89, 'P', 'N', 'G', 0, 0, 0, 0, 1, 2, 3, 4};
    expect(!aegir::datatypes::png::identify(garbage.data(), garbage.size()),
           "a bad signature does not identify");
}

void check_rgba()
{
    /* 2x2, four distinct opaque colours. */
    Bytes const file = png_bytes(2, 2, 6, 8,
                                 rgba_rows(2, 2, {255, 0, 0, 255, 0, 255, 0, 255,
                                                  0, 0, 255, 255, 255, 255, 255, 255}));
    Info info{};
    expect(aegir::datatypes::png::probe(file.data(), file.size(), info), "probe RGBA");
    expect(info.width == 2 && info.height == 2, "RGBA is 2x2");
    expect(info.format == Format::RGBA, "RGBA stays RGBA");
    expect(info.stride == 8, "RGBA strides width*4");
    expect(info.palette_size == 0, "RGBA has no palette");
    expect(!info.transparent, "the normalisation states no index transparency");

    Decoded d;
    expect(aegir::datatypes::png::decode(file.data(), file.size(), d), "decode RGBA");
    Bytes const want = {255, 0, 0, 255, 0, 255, 0, 255,
                        0, 0, 255, 255, 255, 255, 255, 255};
    expect(d.pixels == want, "RGBA pixels land in row order");
    expect(d.info.width == 2 && d.info.stride == 8, "decode states the same size");
    expect(d.palette.empty(), "an RGBA frame has no palette");
    expect(d.stream_size() == want.size(), "the stream is the pixels alone");
}

void check_rgb_normalised()
{
    /* Colour type 2: three channels, no alpha. It must arrive as RGBA, alpha
     * 255 -- the layout the client draws. */
    Bytes const file = png_bytes(2, 1, 2, 8, {0, 10, 20, 30, 40, 50, 60});
    Decoded d;
    expect(aegir::datatypes::png::decode(file.data(), file.size(), d), "decode RGB");
    expect(d.info.format == Format::RGBA, "RGB is normalised to RGBA");
    expect(d.info.stride == 8, "RGB's stride is the RGBA one");
    Bytes const want = {10, 20, 30, 255, 40, 50, 60, 255};
    expect(d.pixels == want, "RGB gains a 255 alpha");
}

void check_palette_normalised()
{
    /* Colour type 3: a palette. The indices resolve to RGBA; no palette
     * survives into the frame (palette_size is zero). */
    Bytes const plte = {0, 0, 0, 10, 20, 30};
    Bytes const file = png_bytes(2, 1, 3, 8, {0, 1, 0}, plte);
    Info info{};
    expect(aegir::datatypes::png::probe(file.data(), file.size(), info), "probe palette");
    expect(info.palette_size == 0 && info.format == Format::RGBA,
           "a palette is resolved, not served");
    Decoded d;
    expect(aegir::datatypes::png::decode(file.data(), file.size(), d), "decode palette");
    Bytes const want = {10, 20, 30, 255, 0, 0, 0, 255};
    expect(d.pixels == want, "palette indices become RGBA");
}

void check_greyscale_normalised()
{
    /* Colour type 0: one channel, replicated across R, G and B. */
    Bytes const file = png_bytes(2, 1, 0, 8, {0, 0, 200});
    Decoded d;
    expect(aegir::datatypes::png::decode(file.data(), file.size(), d), "decode grey");
    Bytes const want = {0, 0, 0, 255, 200, 200, 200, 255};
    expect(d.pixels == want, "grey is replicated to RGB");
}

void check_grey_alpha_normalised()
{
    /* Colour type 4: grey plus an alpha channel; both are carried. */
    Bytes const file = png_bytes(2, 1, 4, 8, {0, 100, 255, 50, 128});
    Decoded d;
    expect(aegir::datatypes::png::decode(file.data(), file.size(), d), "decode grey+alpha");
    Bytes const want = {100, 100, 100, 255, 50, 50, 50, 128};
    expect(d.pixels == want, "grey+alpha keeps the alpha");
}

void check_alpha_preserved()
{
    Bytes const file = png_bytes(2, 1, 6, 8,
                                 {0, 1, 2, 3, 10, 4, 5, 6, 7});
    Decoded d;
    expect(aegir::datatypes::png::decode(file.data(), file.size(), d), "decode RGBA alpha");
    Bytes const want = {1, 2, 3, 10, 4, 5, 6, 7};
    expect(d.pixels == want, "a partial alpha crosses untouched");
}

void check_row_order()
{
    /* 1 pixel wide, three rows: a transposed or badly strided frame shows. */
    Bytes const file = png_bytes(1, 3, 6, 8,
                                 {0, 1, 0, 0, 255, 0, 2, 0, 0, 255, 0, 3, 0, 0, 255});
    Decoded d;
    expect(aegir::datatypes::png::decode(file.data(), file.size(), d), "decode 1x3");
    Bytes const want = {1, 0, 0, 255, 2, 0, 0, 255, 3, 0, 0, 255};
    expect(d.pixels == want, "rows stay in order, not transposed");
}

void check_stream_read()
{
    Bytes const file = png_bytes(2, 2, 6, 8,
                                 rgba_rows(2, 2, {1, 2, 3, 4, 5, 6, 7, 8,
                                                  9, 10, 11, 12, 13, 14, 15, 16}));
    Decoded d;
    expect(aegir::datatypes::png::decode(file.data(), file.size(), d), "decode for the stream");
    Bytes whole(d.stream_size(), 0);
    std::size_t const n = d.read(0, whole.data(), whole.size());
    expect(n == d.pixels.size() && whole == d.pixels,
           "the stream is the pixels, read whole");
    expect(d.read(d.stream_size(), whole.data(), 4) == 0, "a read past the end is empty");
}

void check_malformed()
{
    Bytes const good = png_bytes(2, 1, 6, 8, {0, 1, 2, 3, 4, 5, 6, 7, 8});

    /* The signature is intact but the file is cut in half: identify still says
     * yes, decode says no. */
    Decoded d;
    expect(aegir::datatypes::png::identify(good.data(), good.size()),
           "the good file identifies");
    expect(!aegir::datatypes::png::decode(good.data(), good.size() / 2, d),
           "a half-written PNG does not decode");

    /* A corrupted IDAT byte either breaks the deflate stream or the chunk's
     * CRC, so libpng refuses it. */
    Bytes corrupt = good;
    corrupt[corrupt.size() / 2] ^= 0xff;
    Decoded e;
    expect(!aegir::datatypes::png::decode(corrupt.data(), corrupt.size(), e),
           "a corrupted IDAT does not decode");

    /* A PNG header with no chunks after it. */
    Bytes const sig = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    Decoded f;
    expect(!aegir::datatypes::png::decode(sig.data(), sig.size(), f),
           "a signature alone does not decode");
}

} // namespace

int main()
{
    check_identify();
    check_rgba();
    check_rgb_normalised();
    check_palette_normalised();
    check_greyscale_normalised();
    check_grey_alpha_normalised();
    check_alpha_preserved();
    check_row_order();
    check_stream_read();
    check_malformed();

    std::printf("PNG: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
