/*
 * libpng -- the datatypes PNG class (specs/datatypes.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * PNG through the vendored libpng, the second datatype class. It reads a whole
 * image in memory and states a canonical `Info`/`Decoded`
 * (libs/freestanding/aegir-datatypes/include/aegir/datatypes.h); libpng's own
 * row filters, interlacing and palette stop here and the caller gets chunky
 * bytes.
 *
 * The simplified libpng API (`png_image`) is used on purpose: it owns the read
 * state and the error handling in one call pair, so this file holds no setjmp
 * and no per-chunk callbacks -- the failure of a malformed file comes back as a
 * false, not a longjmp past the class service's stack. The colour type is
 * normalised to `RGBA`, so a palette, an alpha channel and a greyscale PNG all
 * arrive in one layout the client already draws.
 *
 * The class service (apps/hosted/aegir-png-datatype) reads the file through
 * `vfs.namespace` and calls these; the host conformance
 * (scripts/check_png.py) calls them directly against libpng built for the host.
 */

#ifndef AEGIR_PNG_H
#define AEGIR_PNG_H

#include <aegir/datatypes.h>
#include <aegir/datatype/decoded.h>

#include <cstddef>
#include <cstdint>

namespace aegir::datatypes::png {

/* Does this look like a PNG? Checks libpng's signature, not the pixels. */
bool identify(const uint8_t *data, size_t size) noexcept;

/* The image's size and layout, without decoding pixels. Fills `out`'s width,
 * height, format and stride; a PNG has no palette and no index transparency in
 * this normalisation, so those are zero. */
bool probe(const uint8_t *data, size_t size, Info &out) noexcept;

/* Decode the whole image into `out`: its `Info` and its pixels (`stride *
 * height` bytes) as RGBA. The class owns the result and serves it a page at a
 * time through `Decoded::read` (specs/datatypes.md). False, and `out` untouched,
 * on a malformed file. */
bool decode(const uint8_t *data, size_t size, Decoded &out);

} // namespace aegir::datatypes::png

#endif // AEGIR_PNG_H
