/*
 * aegir::filesystem's bridge to the freestanding transport.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The wrapper's public translation unit is the libc++ side and may not include
 * an seL4 header (the seL4 headers declare `strcpy` with C++ linkage and musl's
 * <string.h>, which libc++ pulls in, declares it with C linkage -- one
 * translation unit cannot have both). So the calls that need the transport
 * live here, behind a header of plain types, and the libc++ side builds its
 * std::vector from this.
 */

#ifndef AEGIR_FILESYSTEM_VOLUME_SOURCE_H
#define AEGIR_FILESYSTEM_VOLUME_SOURCE_H

#include <stdint.h>

namespace aegir::filesystem::internal {

/** How many volumes the namespace holds. False when it is not there or
 *  refuses. */
bool volume_count(uint64_t &count) noexcept;

/** One volume's fields, copied into `name` (up to `capacity`, `name_length`
 *  set). False when the index is past the end or the namespace refuses. */
bool describe(uint64_t index, char *name, uint32_t capacity, uint32_t &name_length,
              uint64_t &flags, uint64_t &bound) noexcept;

}  // namespace aegir::filesystem::internal

#endif  // AEGIR_FILESYSTEM_VOLUME_SOURCE_H
