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

#include <aegir/size.h>

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

/** The five English unit names for the decimal digital units `scale_size`
 *  picks (`aegir/size.h`): "bytes", "kB", "MB", "GB", "TB". */
extern char const *const kSizeUnits[5];

/** `bytes` in its largest decimal unit, to one decimal, with `units` --
 *  "8.9 MB", "143.5 kB", "0 bytes". The default table is `kSizeUnits`,
 *  English; the toolkit's `Locale::format_size` is the localized form, which
 *  uses CLDR's unit patterns instead of this table. */
std::string format_size(uint64_t bytes, char const *const units[5] = kSizeUnits);

}  // namespace aegir::filesystem

#endif  // AEGIR_FILESYSTEM_H
