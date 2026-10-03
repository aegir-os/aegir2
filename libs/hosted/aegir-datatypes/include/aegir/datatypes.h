/*
 * Aegir datatypes -- the image ABI a class and its caller share.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A resource library in Aegir is a service (specs/libraries.md), and datatypes
 * is the first: `datatypes` brokers a file to a per-format *class* service, and
 * the class decodes into its own memory and serves the frame a page at a time
 * (specs/datatypes.md). This header is the one thing both sides include: the
 * class states `Info`, the frame stream is pixels then palette, and neither
 * knows what the other is.
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

/* ---- the port protocol (specs/datatypes.md) ----
 *
 * The broker owns `datatypes.main`; a class owns a port the broker made for it.
 * Strings travel in the namespace protocol's shape (aegir/nmspace.h), as the
 * font and launch protocols' do. A method a service does not know is refused
 * with a zero first word, the versioning rule every port keeps.
 */

/* The broker's port, in the class.instance shape every port is named in. */
constexpr char kBrokerPortName[] = "datatypes.main";
constexpr uint32_t kBrokerPortNameLength = sizeof(kBrokerPortName) - 1;

/* `open`: the class name (empty to identify from the file), the file's path and
 * the caller's program directory, as strings; and one capability, the caller's
 * `vfs.namespace`. Answer: 1 and the class port, or 0 for no class. */
constexpr uint32_t kMethodOpen = 1;

/* `close`: no fields. The manager drops the reference (specs/libraries.md). */
constexpr uint32_t kMethodClose = 2;

/* A class port's methods. */
/* `identify`: a path. Answer 1 when this class reads the file, else 0. */
constexpr uint32_t kMethodIdentify = 1;
/* `info`: a path. Answer 1 and `kInfoWords` words, else 0. */
constexpr uint32_t kMethodInfo = 2;
/* `read`: a path, an offset, and one capability -- a page the caller owns.
 * Answer: how many bytes of the frame stream were filled, in the pixels-then-
 * palette order (`stride*height` bytes, then `palette_size` colours of three).
 * The class decodes once and serves by offset, so a picture larger than a page
 * is several calls (specs/datatypes.md). */
constexpr uint32_t kMethodRead = 3;
/* `dispose`: no fields. The class drops the decoded object. Answer 1. */
constexpr uint32_t kMethodDispose = 4;

/* The `info` words after its success word: width, height, format, stride,
 * palette size, and a flags word (bit 0 transparent, bits 1..15 the transparent
 * index). */
constexpr uint32_t kInfoWords = 6;

} // namespace aegir::datatypes

#endif // AEGIR_DATATYPES_H
