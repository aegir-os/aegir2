/*
 * The font port's protocol: what the font service serves (specs/fonts.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the same shape as aegir/clock.h and aegir/launch.h: the
 * font service includes it to serve, a client includes it to call, and neither
 * has to guess what the other meant. The service owns the faces `Sys:Fonts`
 * holds and the rasterizer; a client owns its own atlas and asks for what it
 * needs to fill it (specs/fonts.md). Strings travel in the namespace
 * protocol's shape (aegir/nmspace.h), as con.stream's and a launch's do.
 *
 * A protocol is versioned by refusing methods it does not know, so a client
 * can tell a method from a service that does not implement it: the answer's
 * first word is 0.
 */

#ifndef AEGIR_FONT_H
#define AEGIR_FONT_H

#include <stdint.h>

namespace aegir::font {

/** The font service's port, in the class.instance shape every port is named
 *  in. One service owns it, started by director before any session draws; a
 *  client finds it through its bootstrap block the way it finds log.main. */
constexpr char const kPortName[] = "font.main";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

/** Open a face (specs/fonts.md). Fields after the method:
 *
 *    family   a string: the family the caller asked for ("Noto Sans")
 *    size     one word: the pixel size it wants
 *    bold     one word: 0 or 1
 *    italic   one word: 0 or 1
 *
 *  Answer: one word -- 1 found and a face id after it, 0 when no face matches
 *  (the caller falls back, specs/fonts.md). The id is the service's to choose
 *  and is valid until `close`; a service that cannot answer refuses with 0
 *  rather than guessing a face. */
constexpr uint32_t kMethodOpen = 1;

/** A face's metrics. Fields after the method:
 *
 *    id       one word: from `open`
 *
 *  Answer: 1 and `kMetricsWords` words, or 0 for an id the service does not
 *  know. The words are, in order, the ascent, the descent, the line gap and
 *  the line height, in pixels with the descent negative, the sign FreeType's
 *  face metrics carry (specs/fonts.md). */
constexpr uint32_t kMethodMetrics = 2;
constexpr uint32_t kMetricsWords = 4;

/** Close a face. Fields after the method:
 *
 *    id       one word: from `open`
 *
 *  Answer: 1 closed, 0 for an id the service does not know. */
constexpr uint32_t kMethodClose = 3;

/** Rasterize glyphs into the caller's transfer page (specs/fonts.md). Fields
 *  after the method:
 *
 *    id       one word: from `open`
 *    count    one word: how many codepoints follow
 *    codes    count words, one codepoint each
 *
 *  and one capability, the caller's page. The page is a frame the caller owns,
 *  maps and hands over for this call alone; the service maps a *copy* of the
 *  capability, writes the glyphs into it and unmaps it before answering, so the
 *  page is the caller's again between calls (specs/fonts.md). A call with no
 *  capability is refused with 0 -- the service will not guess where to put a
 *  glyph.
 *
 *  Answer: 1, then how many codepoints were answered, then that many records of
 *  `kGlyphsWordsPerCode` words each, in request order:
 *
 *    advance       horizontal advance in pixels
 *    bearing_x     distance from the pen to the bitmap's left edge
 *    bearing_y     distance from the baseline to the bitmap's top edge
 *    width         bitmap width in pixels, 0 when the face has no glyph
 *    height        bitmap height in pixels
 *    offset        byte offset of the bitmap in the page, `width * height`
 *                  bytes of 8-bit coverage, row-major
 *    present       1 when the face has the codepoint, 0 when it does not
 *
 *  A record whose `present` is 0 has every other field zero: the caller learns
 *  the codepoint is absent rather than asking again. A page holds what it holds
 *  and the answer is trimmed to it, so the caller re-asks for the rest -- the
 *  count answered says how far it got. One answer's records are bounded by
 *  `aegir::ipc::kMaxWords` too; both sides know the envelope. */
constexpr uint32_t kMethodGlyphs = 4;

/** The words one glyph's record takes in a `glyphs` answer. */
constexpr uint32_t kGlyphsWordsPerCode = 7;

/** The transfer page's size: one small frame, the page the caller retypes and
 *  maps, and the service maps a copy of. It bounds how many glyphs one call can
 *  carry, and it is a page and not a policy -- a bigger request is more calls
 *  (specs/fonts.md). */
constexpr uint32_t kTransferPageBytes = 4096;

}  // namespace aegir::font

#endif  // AEGIR_FONT_H
