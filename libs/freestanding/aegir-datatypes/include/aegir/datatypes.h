/*
 * Aegir datatypes -- the protocol a broker and a class serve, and the image
 * ABI both sides share.
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
 * One header, no code, the same shape as aegir/font.h and aegir/launch.h: the
 * broker and the class include it to serve, a client includes it and the
 * hosted client header to call. Nothing here needs the kernel or the C++
 * library, so a freestanding class service can hold it.
 */

#ifndef AEGIR_DATATYPES_H
#define AEGIR_DATATYPES_H

#include <stdint.h>

namespace aegir::datatypes {

/* A pixel layout a class may produce. A class decodes to one of these --
 * canonical chunky -- so planar storage (ILBM) stops at the class and a caller
 * never sees a bit plane. */
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

/* What a class states about an image before the caller reads the frame. */
struct Info {
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::INDEXED;
    /* Bytes from one row's start to the next. The class states it rather than
     * the caller guessing, because the frame is the class's to lay out. */
    uint32_t stride = 0;
    /* Palette entries, when INDEXED; zero otherwise. */
    uint32_t palette_size = 0;
    /* An index is transparent (ILBM masking by colour). */
    bool transparent = false;
    uint16_t transparent_index = 0;
};

/* ---- the port protocol ----
 *
 * The broker owns `datatypes.main`; a class owns `datatypes.class`, installed
 * by whatever started it under the caller's badge (specs/libraries.md: a user
 * resource library runs as the caller). Strings travel in the namespace
 * protocol's shape (aegir/nmspace.h), as the font and launch protocols' do. A
 * method a service does not know is refused with a zero first word.
 */

/* The broker's port, in the class.instance shape every port is named in. */
constexpr char const kBrokerPortName[] = "datatypes.main";
constexpr uint32_t kBrokerPortNameLength = sizeof(kBrokerPortName) - 1;

/* A class's port: the name the class finds its serve endpoint under, installed
 * by the spawner with the capability the opener provided. */
constexpr char const kClassPortName[] = "datatypes.class";
constexpr uint32_t kClassPortNameLength = sizeof(kClassPortName) - 1;

/* `open`: the class name (empty to identify from the file), the file's path and
 * the caller's program directory, as strings; and one capability, the class
 * port the opener made. Answer: 1 and the class's badge, or 0 for no class.
 * `close`: the class's badge; the manager releases the class it started, so a
 * class lives only as long as the open that asked for it -- not for the whole
 * session, which is what let a viewer's classes hold their slots forever. */
constexpr uint32_t kMethodOpen = 1;
constexpr uint32_t kMethodClose = 2;

/* A class port's methods. `identify`: a path; answer 1 when this class reads the
 * file, else 0. `info`: a path; answer 1 and `kInfoWords` words, else 0.
 * `read`: a path, an offset, and one capability -- a page the caller owns;
 * answer how many bytes of the frame were filled. `dispose`: no fields;
 * answer 1. */
constexpr uint32_t kMethodIdentify = 1;
constexpr uint32_t kMethodInfo = 2;
constexpr uint32_t kMethodRead = 3;
constexpr uint32_t kMethodDispose = 4;

/* The `info` words after its success word: width, height, format, stride,
 * palette size, and a flags word (bit 0 transparent, bits 16..31 the
 * transparent index). */
constexpr uint32_t kInfoWords = 6;

} // namespace aegir::datatypes

#endif // AEGIR_DATATYPES_H
