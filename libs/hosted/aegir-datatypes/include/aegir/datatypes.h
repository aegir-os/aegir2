/*
 * Aegir datatypes -- the image ABI a class and its caller share.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A resource library in Aegir is a service (specs/libraries.md), and datatypes
 * is the first: `datatypes` brokers a file to a per-format *class* service, and
 * the class decodes into a frame the caller owns (specs/datatypes.md). This
 * header is the one thing both sides include -- the class states `Info`, the
 * caller provides a `Bitmap`, and neither knows what the other is.
 *
 * A class decodes to a *canonical chunky* layout. Planar storage (ILBM) is the
 * class's business, not the caller's, so every format arrives as one of the
 * `Format` below with the stride the class states. The caller never sees a bit
 * plane, and a renderer handles at most the four layouts here.
 */

#ifndef AEGIR_DATATYPES_H
#define AEGIR_DATATYPES_H

#include <cstddef>
#include <cstdint>

namespace aegir::datatypes {

/* A pixel layout a class may produce. */
enum class Format : uint8_t {
    INDEXED = 0, /* one byte per pixel, an index into the palette */
    RGB = 1,     /* three bytes per pixel: R, G, B */
    RGBA = 2,    /* four bytes per pixel: R, G, B, A */
    GREY = 3,    /* one byte per pixel, luminance */
};

struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

/* What a class states about an image before the caller provides a frame. */
struct Info {
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::INDEXED;
    /* Bytes from one row's start to the next. The class states it rather than
     * the caller guessing, because the frame is the caller's to size and the
     * class's to fill. */
    uint32_t stride = 0;
    /* Palette entries, when INDEXED; zero otherwise. */
    uint32_t palette_size = 0;
    /* An index is transparent (ILBM masking by colour). */
    bool transparent = false;
    uint16_t transparent_index = 0;
};

/* The caller's frame, as the class sees it once mapped: the pixels and, for
 * INDEXED, the palette. The caller owns both and sizes them from `Info`; the
 * class fills them and never allocates. */
struct Bitmap {
    uint8_t *pixels = nullptr;
    size_t pixels_size = 0;
    Color *palette = nullptr;
    size_t palette_size = 0;
};

} // namespace aegir::datatypes

#endif // AEGIR_DATATYPES_H
