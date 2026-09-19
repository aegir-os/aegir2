/*
 * Trinket Basic BiDi support.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Paragraph-level Unicode Bidirectional Algorithm (UBA).
 * Phase 1: Basic support (direction, cursor, selection).
 * Phase 2: Full HarfBuzz shaping.
 */

#ifndef AEGIR_TRINKET_BIDI_H
#define AEGIR_TRINKET_BIDI_H

#include <cstdint>
#include <string>
#include <vector>

namespace aegir::trinket {

enum class BidiDirection { LTR, RTL };

// Paragraph-level BiDi analysis
struct BidiParagraph {
    BidiDirection base_direction;
    std::u32string text;

    // Visual-to-logical and logical-to-visual mapping
    std::vector<int> visual_to_logical() const;
    std::vector<int> logical_to_visual() const;

    // For cursor movement and selection
    int next_cursor_position(int logical_pos, bool forward) const;
    std::pair<int, int> selection_range(int logical_start, int logical_end) const;

    // Reorder runs for rendering
    struct Run {
        int start;      // Logical start
        int length;     // Number of codepoints
        BidiDirection dir;
        int level;      // Embedding level
    };
    std::vector<Run> runs() const;
};

// Analyze a paragraph (call once, cache result)
BidiParagraph analyze_paragraph(std::u32string_view text,
                                 BidiDirection base_dir = BidiDirection::LTR);

// Mirror a character for RTL (brackets, etc.)
char32_t mirror_char(char32_t c);

// Get BiDi class of a character (simplified)
enum class BidiClass {
    L, R, AL, EN, ES, ET, AN, CS, B, S, WS, ON, LRE, LRO, RLE, RLO,
    PDF, NSM, BN
};
BidiClass bidi_class(char32_t c);

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_BIDI_H