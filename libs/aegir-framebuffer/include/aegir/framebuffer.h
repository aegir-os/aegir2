/*
 * The framebuffer port's protocol: what a display driver serves.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the same shape as aegir/block.h and aegir/input.h:
 * the driver includes it to serve, a client includes it to call. The pixels
 * are not in the protocol -- they are the shared window the port serves
 * through, written directly and pushed with `flush` (specs/services.md).
 * What rides in the envelope is the geometry and the mode question.
 */

#ifndef AEGIR_FRAMEBUFFER_H
#define AEGIR_FRAMEBUFFER_H

#include <stdint.h>

namespace aegir::framebuffer {

/** Info: the answer's words are, in order, width and height in pixels, the
 *  stride in bytes, the format (below), and the physical size in millimetres
 *  -- width then height, zero when the device does not say (no EDID, or none
 *  exists to read: a firmware-fixed framebuffer carries no metrics). The
 *  physical size is what a toolkit's scale factor is worked out from:
 *  dots per inch are width * 25.4 / physical width. */
constexpr uint32_t kMethodInfo = 1;
constexpr uint32_t kInfoWords = 6;

/** Flush: the window's pixels, as they stand, go to the screen. No words; the
 *  answer is empty. Not vsynced -- a tear-free flip is the driver's business,
 *  when a device that has one arrives. */
constexpr uint32_t kMethodFlush = 2;

/** Set mode: two words, width and height in pixels. The driver re-points the
 *  scanout at a new resource backed by the same window and answers with the
 *  two words it applied, or two zeros when refused. The only limit is the
 *  window: width * height * bytes-per-pixel past the window's size is what
 *  "refused" means, because the window is the memory the screen is drawn
 *  from. What the new mode shows first is the driver's to decide -- the
 *  virtio-gpu driver repaints its band pattern; a client that owns the
 *  screen writes its own and calls `flush`. */
constexpr uint32_t kMethodSetMode = 3;

/* The pixel formats a driver may answer with, virtio-gpu's own numbers
 * (virtio 1.x, 5.7.6.6): byte order in memory, little-endian. */
constexpr uint32_t kFormatB8G8R8X8 = 2; /* bytes B, G, R, X -- 0x00RRGGBB a word */

}  // namespace aegir::framebuffer

#endif  // AEGIR_FRAMEBUFFER_H
