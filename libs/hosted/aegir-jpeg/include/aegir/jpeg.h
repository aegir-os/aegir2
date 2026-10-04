/*
 * libjpeg-turbo -- the datatypes JPEG class (specs/datatypes.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * JPEG through the vendored libjpeg-turbo, the third datatype class. It reads a
 * whole image in memory and states a canonical `Info`/`Decoded`
 * (libs/freestanding/aegir-datatypes/include/aegir/datatypes.h); libjpeg's own
 * colour conversion, sampling and entropy decoding stop here and the caller
 * gets chunky RGB.
 *
 * libjpeg-turbo's classic API reports a malformed file by calling the error
 * manager's `error_exit`, which must not return -- so a decode runs under a
 * `setjmp` and a bad file comes back as a false, not a dead class service. The
 * colour type is normalised to `RGB`: a greyscale JPEG arrives as three equal
 * channels, which is what the client draws.
 *
 * The class service (apps/hosted/aegir-jpeg-datatype) reads the file through
 * `vfs.namespace` and calls these; the host conformance (scripts/check_jpeg.py)
 * calls them directly against libjpeg-turbo built for the host.
 */

#ifndef AEGIR_JPEG_H
#define AEGIR_JPEG_H

#include <aegir/datatypes.h>
#include <aegir/datatype/decoded.h>

#include <cstddef>
#include <cstdint>

namespace aegir::datatypes::jpeg {

/* Does this look like a JPEG? Checks the SOI marker, not the pixels. */
bool identify(const uint8_t *data, size_t size) noexcept;

/* The image's size and layout, without decoding pixels. Fills `out`'s width,
 * height, format and stride; a JPEG has no palette and no index transparency in
 * this normalisation, so those are zero. */
bool probe(const uint8_t *data, size_t size, Info &out) noexcept;

/* Decode the whole image into `out`: its `Info` and its pixels (`stride *
 * height` bytes) as RGB. The class owns the result and serves it a page at a
 * time through `Decoded::read` (specs/datatypes.md). False, and `out` untouched,
 * on a malformed file. */
bool decode(const uint8_t *data, size_t size, Decoded &out);

} // namespace aegir::datatypes::jpeg

#endif // AEGIR_JPEG_H
