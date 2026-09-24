/*
 * The generated UAX #9 tables' interface (specs/locale.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * bidi_tables.cc is generated from the pinned Unicode Character Database by
 * scripts/gen_bidi_tables.py; this is the boundary between the generated data
 * and the algorithm in bidi.cc.
 */

#ifndef AEGIR_TRINKET_BIDI_TABLES_H
#define AEGIR_TRINKET_BIDI_TABLES_H

#include <aegir/trinket/bidi.h>
#include <cstdint>

namespace aegir::trinket::detail {

/* The Bidi_Class of `c`, from the DerivedBidiClass ranges. */
BidiClass bidi_class_lookup(char32_t c);

/* `c`'s Bidi_Mirroring_Glyph in `*out`, if it has one. */
bool mirror_lookup(char32_t c, char32_t *out);

/* `c`'s Bidi_Paired_Bracket in `*paired` and whether it is an opening bracket
 * in `*open`, if `c` is a bracket. */
bool bracket_lookup(char32_t c, char32_t *paired, bool *open);

} // namespace aegir::trinket::detail

#endif // AEGIR_TRINKET_BIDI_TABLES_H
