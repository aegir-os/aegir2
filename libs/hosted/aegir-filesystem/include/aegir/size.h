/*
 * A byte count scaled to a human-readable unit -- the arithmetic every size
 * format shares (aegir/filesystem.h's format_size, the toolkit Locale's).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Header-only and dependency-free on purpose: the toolkit's Locale is built
 * host-side by its own conformance run, which links no transport, so the
 * scaling lives here rather than in a translation unit it would have to link.
 * The words are not here -- a unit's *name* is a locale's business -- only
 * which unit a value falls in, which is the same everywhere.
 */

#ifndef AEGIR_SIZE_H
#define AEGIR_SIZE_H

#include <cstdint>

namespace aegir::filesystem {

/** A byte count broken into the largest decimal digital unit it fills, to one
 *  decimal -- what `df -H` writes. `unit` is the unit's ordinal: 0 for a value
 *  under 1000 (bytes), then kB, MB, GB, TB; `tenths` is the fraction 0..9. The
 *  scale is 1000, CLDR's digital units being decimal (it publishes no binary
 *  kibibyte), so a caller names `unit` from a table -- English "bytes/kB/..."
 *  here, or a locale's CLDR patterns. */
struct ScaledSize {
    std::uint64_t whole;  /* the value in its unit */
    std::uint64_t tenths; /* the fraction of that unit, 0..9 */
    unsigned unit;        /* the unit's ordinal (0 bytes .. 4 TB) */
};

inline ScaledSize scale_size(std::uint64_t bytes) noexcept
{
    ScaledSize size{bytes, 0, 0};
    while (size.unit < 4 && size.whole >= 1000) {
        size.tenths = size.whole % 1000;
        size.whole /= 1000;
        ++size.unit;
    }
    /* The remainder of the last division is the fraction of the unit shown. */
    if (size.unit > 0) {
        size.tenths = size.tenths * 10 / 1000;
    }
    return size;
}

}  // namespace aegir::filesystem

#endif  // AEGIR_SIZE_H
