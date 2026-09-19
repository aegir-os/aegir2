/*
 * Trinket Basic BiDi implementation.
 */

#include <aegir/trinket/bidi.h>
#include <aegir/trinket/unicode.h>
#include <algorithm>
#include <vector>

namespace aegir::trinket {

BidiParagraph::Run::Run() = default;
BidiParagraph::Run::Run(int s, int l, BidiDirection d, int lev)
    : start(s), length(l), dir(d), level(lev) {}

BidiParagraph analyze_paragraph(std::u32string_view text, BidiDirection base_dir) {
    BidiParagraph para;
    para.text = std::u32string(text);
    para.base_direction = base_dir;

    // Simplified BiDi - just determine runs based on character types
    // Real implementation would follow full UBA (UAX #9)

    int start = 0;
    int len = static_cast<int>(text.size());
    BidiDirection current_dir = base_dir;
    int current_level = (base_dir == BidiDirection::RTL) ? 1 : 0;

    while (start < len) {
        int run_start = start;
        BidiDirection run_dir = current_dir;

        // Find run of same directionality
        while (start < len) {
            BidiClass bc = bidi_class(text[start]);
            BidiDirection char_dir = (bc == BidiClass::R || bc == BidiClass::AL)
                ? BidiDirection::RTL
                : (bc == BidiClass::L ? BidiDirection::LTR : current_dir);

            if (char_dir != run_dir && bc != BidiClass::NSM && bc != BidiClass::BN) {
                break;
            }
            start++;
        }

        if (start > run_start) {
            para.runs().push_back(BidiParagraph::Run(run_start, start - run_start, run_dir, current_level));
        }
    }

    return para;
}

BidiClass bidi_class(char32_t c) {
    // Simplified BiDi class detection
    if (c >= 0x0000 && c <= 0x001F) return BidiClass::BN;
    if (c >= 0x0020 && c <= 0x0020) return BidiClass::WS;
    if (c >= 0x0021 && c <= 0x002C) return BidiClass::ON;
    if (c >= 0x002D && c <= 0x002D) return BidiClass::ES;
    if (c >= 0x002E && c <= 0x002E) return BidiClass::CS;
    if (c >= 0x002F && c <= 0x002F) return BidiClass::ES;
    if (c >= 0x0030 && c <= 0x0039) return BidiClass::EN;
    if (c >= 0x003A && c <= 0x0040) return BidiClass::ON;
    if (c >= 0x0041 && c <= 0x005A) return BidiClass::L;
    if (c >= 0x005B && c <= 0x0060) return BidiClass::ON;
    if (c >= 0x0061 && c <= 0x007A) return BidiClass::L;
    if (c >= 0x007B && c <= 0x007E) return BidiClass::ON;
    if (c == 0x007F) return BidiClass::BN;

    // Arabic
    if (c >= 0x0600 && c <= 0x06FF) return BidiClass::AL;
    if (c >= 0x0750 && c <= 0x077F) return BidiClass::AL;
    if (c >= 0x08A0 && c <= 0x08FF) return BidiClass::AL;
    if (c >= 0xFB50 && c <= 0xFDFF) return BidiClass::AL;
    if (c >= 0xFE70 && c <= 0xFEFF) return BidiClass::AL;

    // Hebrew
    if (c >= 0x0590 && c <= 0x05FF) return BidiClass::R;

    // Default
    return BidiClass::ON;
}

char32_t mirror_char(char32_t c) {
    // Bracket mirroring
    switch (c) {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        case '<': return '>';
        case '>': return '<';
        case 0x2039: return 0x203A;  // ‹ ›
        case 0x203A: return 0x2039;
        case 0x3008: return 0x3009;  // 〈 〉
        case 0x3009: return 0x3008;
        case 0x300A: return 0x300B;  // 《 》
        case 0x300B: return 0x300A;
    }
    return c;
}

std::vector<int> BidiParagraph::visual_to_logical() const {
    std::vector<int> mapping;
    for (const auto& run : runs()) {
        if (run.dir == BidiDirection::RTL) {
            for (int i = run.length - 1; i >= 0; --i) {
                mapping.push_back(run.start + i);
            }
        } else {
            for (int i = 0; i < run.length; ++i) {
                mapping.push_back(run.start + i);
            }
        }
    }
    return mapping;
}

std::vector<int> BidiParagraph::logical_to_visual() const {
    std::vector<int> mapping(text.size());
    int visual_idx = 0;
    for (const auto& run : runs()) {
        if (run.dir == BidiDirection::RTL) {
            for (int i = run.length - 1; i >= 0; --i) {
                mapping[run.start + i] = visual_idx++;
            }
        } else {
            for (int i = 0; i < run.length; ++i) {
                mapping[run.start + i] = visual_idx++;
            }
        }
    }
    return mapping;
}

int BidiParagraph::next_cursor_position(int logical_pos, bool forward) const {
    auto l2v = logical_to_visual();
    int visual_pos = l2v[logical_pos];
    visual_pos += forward ? 1 : -1;
    if (visual_pos < 0) visual_pos = 0;
    if (visual_pos >= static_cast<int>(text.size())) visual_pos = text.size() - 1;
    auto v2l = visual_to_logical();
    return v2l[visual_pos];
}

std::pair<int, int> BidiParagraph::selection_range(int logical_start, int logical_end) const {
    if (logical_start > logical_end) std::swap(logical_start, logical_end);
    auto l2v = logical_to_visual();
    int v_start = l2v[logical_start];
    int v_end = l2v[logical_end];
    return {std::min(v_start, v_end), std::max(v_start, v_end)};
}

std::vector<BidiParagraph::Run> BidiParagraph::runs() const {
    // Return cached runs - simplified
    return {};
}

} // namespace aegir::trinket