/*
 * Trinket Unicode Bidirectional Algorithm (UAX #9).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The paragraph-level algorithm, through L2, in full: explicit levels and
 * isolating run sequences, the weak, neutral and implicit rules, bracket
 * pairs, and reordering. The tables come from the pinned Unicode Character
 * Database, generated at build time (specs/locale.md).
 */

#ifndef AEGIR_TRINKET_BIDI_H
#define AEGIR_TRINKET_BIDI_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegir::trinket {

enum class BidiDirection { LTR, RTL };

/* The Unicode Bidi_Class property, with the four isolates UAX #9 X10 needs. */
enum class BidiClass {
    L, R, AL, EN, ES, ET, AN, CS, B, S, WS, ON,
    LRE, LRO, RLE, RLO, PDF, NSM, BN, LRI, RLI, FSI, PDI
};

/* A resolved run: `start` and `length` are logical positions, `level` is the
 * resolved embedding level, and `dir` is its parity. */
struct BidiRun {
    int start;
    int length;
    BidiDirection dir;
    int level;
};

/* A paragraph's analysis. `text` is the input, `levels[i]` the resolved level
 * of `text[i]`, and `order[v]` the logical index at visual position `v`
 * (rule L2). Characters removed by X9 keep a level but are absent from
 * `order`. */
struct BidiParagraph {
    BidiDirection base_direction = BidiDirection::LTR;
    int paragraph_level = 0;
    std::u32string text;
    std::vector<uint8_t> levels;
    std::vector<int> order;

    /* The maximal runs of equal level, in logical order. */
    std::vector<BidiRun> runs() const;
    /* `order`, and its inverse. */
    std::vector<int> visual_to_logical() const { return order; }
    std::vector<int> logical_to_visual() const;
    /* The logical position one visual step from `logical_pos`. */
    int next_cursor_position(int logical_pos, bool forward) const;
    /* The visual range two logical positions span. */
    std::pair<int, int> selection_range(int logical_start, int logical_end) const;
};

/* Analyze one paragraph. A null `base` selects the paragraph level by rules
 * P2/P3 (the first strong character); a direction forces it. */
BidiParagraph analyze_paragraph(std::u32string_view text,
                                std::optional<BidiDirection> base = std::nullopt);

/* The mirror of `c` under Bidi_Mirroring, or `c` when it has none. */
char32_t mirror_char(char32_t c);

/* The Bidi_Class of `c`. */
BidiClass bidi_class(char32_t c);

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_BIDI_H
