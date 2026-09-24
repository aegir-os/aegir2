/*
 * The generated character-width table's interface (specs/terminal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * width_tables.cc is generated from the pinned Unicode Character Database by
 * scripts/gen_width_tables.py; this is the boundary between the generated data
 * and the grid in terminal_view.cc.
 */

#ifndef AEGIR_TRINKET_WIDTH_TABLES_H
#define AEGIR_TRINKET_WIDTH_TABLES_H

#include <cstdint>

namespace aegir::trinket::detail {

/* How many cells `c` occupies in a monospace grid: 0 for a combining mark, a
 * format character or a control, 2 for an East Asian wide or fullwidth
 * character, 1 for everything else. */
int char_width(char32_t c);

} // namespace aegir::trinket::detail

#endif // AEGIR_TRINKET_WIDTH_TABLES_H
