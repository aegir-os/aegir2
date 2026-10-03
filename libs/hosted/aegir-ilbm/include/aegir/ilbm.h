/*
 * ILBM -- the Amiga's own picture format, decoded for the datatypes system.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The first datatype class (specs/datatypes.md): IFF InterLeaved BitMap, the
 * Amiga's picture format, with no third-party dependency. It reads a whole
 * image in memory and states a canonical `Info`/`Bitmap`
 * (libs/hosted/aegir-datatypes/include/aegir/datatypes.h); planar bit-planes
 * stop here and the caller gets chunky bytes.
 *
 * The class service (apps/hosted/aegir-ilbm) reads the file through
 * `vfs.namespace` and calls these; the host conformance
 * (scripts/check_ilbm.py) calls them directly, so the decode is proved without
 * a service.
 */

#ifndef AEGIR_ILBM_H
#define AEGIR_ILBM_H

#include <aegir/datatypes.h>
#include <aegir/datatype/decoded.h>

#include <cstddef>
#include <cstdint>

namespace aegir::datatypes::ilbm {

/* Does this look like a FORM/ILBM our decoder can read? Scans the FORM type
 * and the BMHD, not the pixels. */
bool identify(const uint8_t *data, size_t size) noexcept;

/* The image's size, layout and palette, without decoding pixels. Fills `out`'s
 * width, height, format, stride, palette size and transparency. */
bool probe(const uint8_t *data, size_t size, Info &out) noexcept;

/* Decode the whole image into `out`: its `Info`, its pixels (`stride * height`
 * bytes) and its palette. The class owns the result and serves it a page at a
 * time through `Decoded::read` (specs/datatypes.md). False, and `out`
 * untouched, on a malformed file. */
bool decode(const uint8_t *data, size_t size, Decoded &out);

} // namespace aegir::datatypes::ilbm

#endif // AEGIR_ILBM_H
