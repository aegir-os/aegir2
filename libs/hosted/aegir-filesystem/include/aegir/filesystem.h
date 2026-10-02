/*
 * aegir::filesystem: Aegir's filesystem beside std::filesystem.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * std::filesystem gives a program the standard C++ face (specs/cxx.md step 5);
 * this gives it Aegir's, for what std::filesystem has no path for --
 * enumerating the volumes the namespace holds, and (next) resolving an Aegir
 * path to its volume and the rest. It is hosted because its error model is
 * std::filesystem's: the throwing overloads raise std::system_error, the
 * error_code overloads report, and neither is the bool a freestanding caller
 * wants. The transport under it is aegir::vfs, which a freestanding service
 * can link on its own.
 */

#ifndef AEGIR_FILESYSTEM_H
#define AEGIR_FILESYSTEM_H

#include <string>
#include <system_error>
#include <vector>

namespace aegir::filesystem {

/** One volume the namespace holds (aegir/nmspace.h's Row, in std types). */
struct VolumeInfo {
    std::string name;
    bool read_only = false;
    bool bound = false;
};

/** Every volume the namespace holds, in its order. The error_code overload
 *  reports a refusal and returns what it managed to read; the other raises
 *  std::system_error. */
std::vector<VolumeInfo> volumes();
std::vector<VolumeInfo> volumes(std::error_code &error);

/** A byte count broken into the largest binary unit it fills, to one decimal
 *  -- what `df -h` writes. `unit` is the unit's ordinal: 0 for a value under
 *  1024 (bytes), then KiB, MiB, GiB, TiB; `tenths` is the fraction 0..9. It
 *  is pure arithmetic and names no words: the unit's *name* and the number's
 *  own spelling are a locale's, so a caller that has one formats from these
 *  fields (`format_size` is the English default). */
struct ScaledSize {
    uint64_t whole;  /* the value in its unit */
    uint64_t tenths; /* the fraction of that unit, 0..9 */
    unsigned unit;   /* the unit's ordinal (0 Bytes .. 4 TiB) */
};

/** `bytes` scaled to the largest binary unit it fills, to one decimal. */
ScaledSize scale_size(uint64_t bytes) noexcept;

/** The five English unit names: "Bytes", "KiB", "MiB", "GiB", "TiB". A locale
 *  passes its own table to `format_size`; these are the ones that will move
 *  into one. */
extern char const *const kSizeUnits[5];

/** `bytes` as `df -h` writes it with `units` -- "8.9 MiB", "143.5 KiB",
 *  "0 Bytes". The default table is `kSizeUnits`, English; a locale passes its
 *  own names, and may format the number from `scale_size` for its own decimal
 *  mark. */
std::string format_size(uint64_t bytes, char const *const units[5] = kSizeUnits);

}  // namespace aegir::filesystem

#endif  // AEGIR_FILESYSTEM_H
