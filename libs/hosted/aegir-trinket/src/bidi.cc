/*
 * Trinket Unicode Bidirectional Algorithm (UAX #9) -- implementation.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The steps are the annex's, in its order: P2/P3, X1-X10, W1-W7, N0-N2, I1-I2,
 * L1-L2. The tables are generated (bidi_tables.cc, specs/locale.md).
 */

#include <aegir/trinket/bidi.h>

#include "bidi_tables.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace aegir::trinket {
namespace {

using detail::bidi_class_lookup;
using detail::bracket_lookup;
using detail::mirror_lookup;

/* X1: the maximum explicit embedding level. */
constexpr int kMaxLevel = 125;

/* A character removed by X9 -- the explicit embedding controls and the
 * boundary neutrals. They keep a level but take no part after this. */
bool is_removed(BidiClass c) noexcept
{
    switch (c) {
    case BidiClass::RLE:
    case BidiClass::LRE:
    case BidiClass::RLO:
    case BidiClass::LRO:
    case BidiClass::PDF:
    case BidiClass::BN:
        return true;
    default:
        return false;
    }
}

bool is_isolate(BidiClass c) noexcept
{
    return c == BidiClass::LRI || c == BidiClass::RLI || c == BidiClass::FSI;
}

bool is_isolate_or_pdi(BidiClass c) noexcept
{
    return is_isolate(c) || c == BidiClass::PDI;
}

/* The first strong direction (0 L, 1 R) in [from, to), skipping the contents
 * of isolates (P2/P3 and FSI). -1 when there is none. */
int first_strong(std::u32string_view text, std::vector<BidiClass> const &types,
                 int from, int to) noexcept
{
    static_cast<void>(text);
    int depth = 0;
    for (int i = from; i < to; ++i) {
        BidiClass const c = types[i];
        if (is_isolate(c)) {
            ++depth;
        } else if (c == BidiClass::PDI) {
            if (depth > 0) {
                --depth;
            }
        } else if (depth == 0) {
            if (c == BidiClass::L) {
                return 0;
            }
            if (c == BidiClass::R || c == BidiClass::AL) {
                return 1;
            }
        }
    }
    return -1;
}

/* The direction a resolved class contributes to a neutral's surroundings: EN
 * and AN are treated as R (N1/N2), and the weak and neutral classes are not
 * strong. Returns 0 L, 1 R, -1 neither. */
int strong_contrib(BidiClass c) noexcept
{
    if (c == BidiClass::L) {
        return 0;
    }
    if (c == BidiClass::R || c == BidiClass::EN || c == BidiClass::AN) {
        return 1;
    }
    return -1;
}

bool is_neutral(BidiClass c) noexcept
{
    return strong_contrib(c) < 0;
}

BidiClass class_of(int dir) noexcept
{
    return dir == 0 ? BidiClass::L : BidiClass::R;
}

/* The least odd level greater than `level`, and the least even level greater
 * than it (X2/X3 and the isolate equivalents). */
int least_odd_greater(int level) noexcept { return (level + 1) | 1; }
int least_even_greater(int level) noexcept { return (level + 2) & ~1; }

/* The paragraph level and the direction of `sos`/`eos` from a pair of levels:
 * the higher level's parity (X10). */
int higher_parity(int a, int b) noexcept { return (a > b ? a : b) & 1; }

/* One entry of the explicit-level stack (X1-X8). `override` is 0 none, 1 L,
 * 2 R; `isolate` marks an isolate entry. */
struct StackEntry {
    int level;
    int override;
    bool isolate;
};

/* The level runs of the retained sequence, in retained coordinates. */
struct LevelRun {
    int begin;
    int end;
    int level;
};

} // namespace

BidiClass bidi_class(char32_t c)
{
    return bidi_class_lookup(c);
}

char32_t mirror_char(char32_t c)
{
    char32_t mirrored = c;
    if (mirror_lookup(c, &mirrored)) {
        return mirrored;
    }
    return c;
}

BidiParagraph analyze_paragraph(std::u32string_view text,
                                std::optional<BidiDirection> base)
{
    int const n = static_cast<int>(text.size());
    BidiParagraph para;
    para.text = std::u32string(text);
    para.levels.assign(static_cast<std::size_t>(n) < 1 ? 0 : static_cast<std::size_t>(n),
                       0);

    std::vector<BidiClass> types(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        types[i] = bidi_class_lookup(text[i]);
    }

    /* P2/P3: the paragraph level, from the caller or the first strong. */
    int paragraph_level = 0;
    if (base.has_value()) {
        paragraph_level = *base == BidiDirection::RTL ? 1 : 0;
    } else {
        int const d = first_strong(text, types, 0, n);
        paragraph_level = d < 0 ? 0 : d;
    }
    para.paragraph_level = paragraph_level;
    para.base_direction = (paragraph_level & 1) != 0 ? BidiDirection::RTL
                                                     : BidiDirection::LTR;

    std::vector<int> levels(static_cast<std::size_t>(n), paragraph_level);
    std::vector<BidiClass> resolved = types;

    /* The matching PDI of each isolate initiator, for X10 and FSI. -1 for an
     * initiator with no matching PDI (its isolate runs to the paragraph end). */
    std::vector<int> pdi_match(static_cast<std::size_t>(n), -1);
    {
        std::vector<int> open;
        for (int i = 0; i < n; ++i) {
            if (is_isolate(types[i])) {
                open.push_back(i);
            } else if (types[i] == BidiClass::PDI && !open.empty()) {
                pdi_match[static_cast<std::size_t>(open.back())] = i;
                open.pop_back();
            }
        }
    }

    /* X1-X8: explicit levels and directions, with overflow and validity. */
    {
        std::vector<StackEntry> stack;
        stack.push_back({paragraph_level, 0, false});
        int overflow_isolate = 0;
        int overflow_embedding = 0;
        int valid_isolate = 0;

        for (int i = 0; i < n; ++i) {
            BidiClass const c = types[i];
            switch (c) {
            case BidiClass::RLE:
            case BidiClass::LRE:
            case BidiClass::RLO:
            case BidiClass::LRO: {
                levels[i] = stack.back().level;
                bool const right = c == BidiClass::RLE || c == BidiClass::RLO;
                int const next = right ? least_odd_greater(stack.back().level)
                                       : least_even_greater(stack.back().level);
                int const override = c == BidiClass::RLO   ? 2
                                     : c == BidiClass::LRO ? 1
                                                           : 0;
                if (next <= kMaxLevel && overflow_isolate == 0 &&
                    overflow_embedding == 0) {
                    stack.push_back({next, override, false});
                } else if (overflow_isolate == 0) {
                    ++overflow_embedding;
                }
                break;
            }
            case BidiClass::RLI:
            case BidiClass::LRI:
            case BidiClass::FSI: {
                int dir = c == BidiClass::RLI ? 1 : 0;
                if (c == BidiClass::FSI) {
                    int const end = pdi_match[i] >= 0 ? pdi_match[i] : n;
                    int const d = first_strong(text, types, i + 1, end);
                    /* P3's default when no strong character is found is LTR. */
                    dir = d < 0 ? 0 : d;
                }
                levels[i] = stack.back().level;
                if (stack.back().override == 1) {
                    resolved[i] = BidiClass::L;
                } else if (stack.back().override == 2) {
                    resolved[i] = BidiClass::R;
                }
                int const next = dir != 0 ? least_odd_greater(stack.back().level)
                                          : least_even_greater(stack.back().level);
                if (next <= kMaxLevel && overflow_isolate == 0 &&
                    overflow_embedding == 0) {
                    stack.push_back({next, 0, true});
                    ++valid_isolate;
                } else {
                    ++overflow_isolate;
                }
                break;
            }
            case BidiClass::PDI: {
                if (overflow_isolate > 0) {
                    --overflow_isolate;
                } else if (valid_isolate > 0) {
                    overflow_embedding = 0;
                    while (!stack.back().isolate) {
                        stack.pop_back();
                    }
                    stack.pop_back();
                    --valid_isolate;
                }
                levels[i] = stack.back().level;
                if (stack.back().override == 1) {
                    resolved[i] = BidiClass::L;
                } else if (stack.back().override == 2) {
                    resolved[i] = BidiClass::R;
                }
                break;
            }            case BidiClass::PDF: {
                if (overflow_isolate > 0) {
                    /* nothing */
                } else if (overflow_embedding > 0) {
                    --overflow_embedding;
                } else if (stack.size() >= 2 && !stack.back().isolate) {
                    stack.pop_back();
                }
                levels[i] = stack.back().level;
                break;
            }
            case BidiClass::B: {
                /* A paragraph separator ends the paragraph's X rules. */
                levels[i] = paragraph_level;
                stack.assign(1, StackEntry{paragraph_level, 0, false});
                overflow_isolate = 0;
                overflow_embedding = 0;
                valid_isolate = 0;
                break;
            }
            default: {
                levels[i] = stack.back().level;
                if (stack.back().override == 1) {
                    resolved[i] = BidiClass::L;
                } else if (stack.back().override == 2) {
                    resolved[i] = BidiClass::R;
                }
                break;
            }
            }
        }
    }

    /* The explicit levels as X assigns them. Rules W/N/I and the sos/eos
     * boundaries read these, never a level I1/I2 has already changed. */
    std::vector<int> const base_levels = levels;

    /* X9: the retained sequence, and the level runs over it. */
    std::vector<int> retained;
    for (int i = 0; i < n; ++i) {
        if (!is_removed(types[i])) {
            retained.push_back(i);
        }
    }
    int const m = static_cast<int>(retained.size());

    std::vector<LevelRun> runs;
    for (int i = 0; i < m;) {
        int j = i;
        while (j < m && levels[retained[j]] == levels[retained[i]]) {
            ++j;
        }
        runs.push_back({i, j, levels[retained[i]]});
        i = j;
    }

    /* X10/BD13: the isolating run sequences -- chains of level runs joined
     * across a matching isolate initiator/PDI. */
    std::vector<int> position(static_cast<std::size_t>(n), -1);
    for (int k = 0; k < m; ++k) {
        position[static_cast<std::size_t>(retained[k])] = k;
    }
    std::vector<int> run_of(static_cast<std::size_t>(m));
    for (std::size_t r = 0; r < runs.size(); ++r) {
        for (int k = runs[r].begin; k < runs[r].end; ++k) {
            run_of[static_cast<std::size_t>(k)] = static_cast<int>(r);
        }
    }
    std::vector<int> successor(runs.size(), -1);
    std::vector<int> predecessor(runs.size(), -1);
    for (std::size_t r = 0; r < runs.size(); ++r) {
        int const last = retained[static_cast<std::size_t>(runs[r].end - 1)];
        if (is_isolate(types[last]) && pdi_match[static_cast<std::size_t>(last)] >= 0) {
            int const k = position[static_cast<std::size_t>(
                pdi_match[static_cast<std::size_t>(last)])];
            if (k >= 0) {
                int const next = run_of[static_cast<std::size_t>(k)];
                successor[r] = next;
                predecessor[static_cast<std::size_t>(next)] = static_cast<int>(r);
            }
        }
    }

    for (std::size_t start = 0; start < runs.size(); ++start) {
        if (predecessor[start] != -1) {
            continue;
        }
        /* The sequence's characters, in order. */
        std::vector<int> seq;
        int last_run = static_cast<int>(start);
        for (int r = static_cast<int>(start); r != -1; r = successor[static_cast<std::size_t>(r)]) {
            last_run = r;
            for (int k = runs[static_cast<std::size_t>(r)].begin;
                 k < runs[static_cast<std::size_t>(r)].end; ++k) {
                seq.push_back(retained[static_cast<std::size_t>(k)]);
            }
        }
        int const level = runs[start].level;
        int const first_k = runs[start].begin;
        int const prev_level =
            first_k > 0 ? base_levels[retained[static_cast<std::size_t>(first_k - 1)]]
                        : paragraph_level;
        int const sos = higher_parity(level, prev_level);
        int const last_k = runs[static_cast<std::size_t>(last_run)].end - 1;
        int const last_char = retained[static_cast<std::size_t>(last_k)];
        /* A sequence that ends on an isolate initiator has no matching PDI in
         * it; its isolate runs to the paragraph's end, so eos sees the
         * paragraph level, not the isolate's contents (X10/BD13). */
        int const next_level =
            is_isolate(types[static_cast<std::size_t>(last_char)])
                ? paragraph_level
                : (last_k + 1 < m
                       ? base_levels[retained[static_cast<std::size_t>(last_k + 1)]]
                       : paragraph_level);
        int const eos = higher_parity(level, next_level);

        std::size_t const count = seq.size();
        std::vector<BidiClass> st(count);
        for (std::size_t k = 0; k < count; ++k) {
            st[k] = resolved[static_cast<std::size_t>(seq[k])];
        }

        /* W1: NSM takes the previous type, or ON after an isolate or PDI. */
        for (std::size_t k = 0; k < count; ++k) {
            if (st[k] != BidiClass::NSM) {
                continue;
            }
            if (k == 0) {
                st[k] = class_of(sos);
            } else if (is_isolate_or_pdi(st[k - 1])) {
                st[k] = BidiClass::ON;
            } else {
                st[k] = st[k - 1];
            }
        }

        /* W2: EN after an AL strong type becomes AN. */
        {
            BidiClass last = class_of(sos);
            for (std::size_t k = 0; k < count; ++k) {
                if (st[k] == BidiClass::L || st[k] == BidiClass::R ||
                    st[k] == BidiClass::AL) {
                    last = st[k];
                } else if (st[k] == BidiClass::EN && last == BidiClass::AL) {
                    st[k] = BidiClass::AN;
                }
            }
        }

        /* W3: AL becomes R. */
        for (std::size_t k = 0; k < count; ++k) {
            if (st[k] == BidiClass::AL) {
                st[k] = BidiClass::R;
            }
        }

        /* W4: a separator between two numbers of one kind joins them. */
        for (std::size_t k = 1; k + 1 < count; ++k) {
            if (st[k] == BidiClass::ES && st[k - 1] == BidiClass::EN &&
                st[k + 1] == BidiClass::EN) {
                st[k] = BidiClass::EN;
            } else if (st[k] == BidiClass::CS) {
                if (st[k - 1] == BidiClass::EN && st[k + 1] == BidiClass::EN) {
                    st[k] = BidiClass::EN;
                } else if (st[k - 1] == BidiClass::AN && st[k + 1] == BidiClass::AN) {
                    st[k] = BidiClass::AN;
                }
            }
        }

        /* W5: a run of ET touching an EN becomes EN. */
        for (std::size_t k = 0; k < count;) {
            if (st[k] != BidiClass::ET) {
                ++k;
                continue;
            }
            std::size_t j = k;
            while (j < count && st[j] == BidiClass::ET) {
                ++j;
            }
            bool const touches =
                (k > 0 && st[k - 1] == BidiClass::EN) ||
                (j < count && st[j] == BidiClass::EN);
            if (touches) {
                for (std::size_t x = k; x < j; ++x) {
                    st[x] = BidiClass::EN;
                }
            }
            k = j;
        }

        /* W6: the rest of ES/ET/CS is ON. */
        for (std::size_t k = 0; k < count; ++k) {
            if (st[k] == BidiClass::ES || st[k] == BidiClass::ET ||
                st[k] == BidiClass::CS) {
                st[k] = BidiClass::ON;
            }
        }

        /* W7: EN after an L strong type becomes L. */
        {
            BidiClass last = class_of(sos);
            for (std::size_t k = 0; k < count; ++k) {
                if (st[k] == BidiClass::L || st[k] == BidiClass::R) {
                    last = st[k];
                } else if (st[k] == BidiClass::EN && last == BidiClass::L) {
                    st[k] = BidiClass::L;
                }
            }
        }

        /* N0: bracket pairs (BD16), then their directions. */
        {
            std::vector<std::pair<int, int>> pairs;
            {
                std::vector<std::pair<int, char32_t>> open;
                for (int k = 0; k < static_cast<int>(count); ++k) {
                    if (st[static_cast<std::size_t>(k)] != BidiClass::ON) {
                        continue;
                    }
                    char32_t paired = 0;
                    bool is_open = false;
                    if (!bracket_lookup(text[static_cast<std::size_t>(seq[k])], &paired,
                                        &is_open)) {
                        continue;
                    }
                    auto canonical = [](char32_t cp) {
                        if (cp == 0x2329) {
                            return static_cast<char32_t>(0x3008);
                        }
                        if (cp == 0x232A) {
                            return static_cast<char32_t>(0x3009);
                        }
                        return cp;
                    };
                    char32_t const expected = canonical(paired);
                    if (is_open) {
                        /* BD16: a 64th opening bracket empties the list. */
                        if (open.size() >= 63) {
                            pairs.clear();
                            break;
                        }
                        open.push_back({k, expected});
                    } else {
                        int found = -1;
                        for (int d = static_cast<int>(open.size()) - 1; d >= 0; --d) {
                            if (open[static_cast<std::size_t>(d)].second ==
                                canonical(text[static_cast<std::size_t>(seq[k])])) {
                                found = d;
                                break;
                            }
                        }
                        if (found >= 0) {
                            pairs.push_back(
                                {open[static_cast<std::size_t>(found)].first, k});
                            open.resize(static_cast<std::size_t>(found));
                        }
                    }
                }
            }
            std::sort(pairs.begin(), pairs.end());

            int const e = level & 1;
            for (auto const &pair : pairs) {
                int const o = pair.first;
                int const cl = pair.second;
                /* N0 looks at every strong type inside, and a match with the
                 * embedding direction wins over one against it. */
                bool matches_embedding = false;
                bool has_opposite = false;
                for (int k = o + 1; k < cl; ++k) {
                    int const s = strong_contrib(st[static_cast<std::size_t>(k)]);
                    if (s < 0) {
                        continue;
                    }
                    if (s == e) {
                        matches_embedding = true;
                        break;
                    }
                    has_opposite = true;
                }
                int dir;
                if (matches_embedding) {
                    dir = e;
                } else if (!has_opposite) {
                    /* No strong type inside: N0 leaves the pair alone, and
                     * N1/N2 resolve it like any neutral. */
                    continue;
                } else {
                    int preceding = -1;
                    for (int k = o - 1; k >= 0; --k) {
                        int const s = strong_contrib(st[static_cast<std::size_t>(k)]);
                        if (s >= 0) {
                            preceding = s;
                            break;
                        }
                    }
                    if (preceding < 0) {
                        preceding = sos;
                    }
                    /* Every strong type inside is opposite the embedding
                     * direction; context is established when the preceding
                     * strong type is that same opposite direction. */
                    int const opposite = e == 0 ? 1 : 0;
                    dir = preceding == opposite ? opposite : e;
                }
                BidiClass const bracket = dir != 0 ? BidiClass::R : BidiClass::L;
                st[static_cast<std::size_t>(o)] = bracket;
                st[static_cast<std::size_t>(cl)] = bracket;
                /* NSMs that were NSM before W1 and follow the bracket take its
                 * type (N0). */
                for (int base : {o, cl}) {
                    for (int k = base + 1;
                         k < static_cast<int>(count) &&
                         types[static_cast<std::size_t>(seq[k])] == BidiClass::NSM;
                         ++k) {
                        st[static_cast<std::size_t>(k)] = bracket;
                    }
                }
            }
        }

        /* N1/N2: a run of neutrals takes its surroundings' shared direction, or
         * the embedding direction. */
        for (std::size_t k = 0; k < count;) {
            if (!is_neutral(st[k])) {
                ++k;
                continue;
            }
            std::size_t j = k;
            while (j < count && is_neutral(st[j])) {
                ++j;
            }
            int const left =
                k == 0 ? sos : (strong_contrib(st[k - 1]) == 0 ? 0 : 1);
            int const right =
                j == count ? eos : (strong_contrib(st[j]) == 0 ? 0 : 1);
            BidiClass const dir = left == right ? class_of(left) : class_of(level & 1);
            for (std::size_t x = k; x < j; ++x) {
                st[x] = dir;
            }
            k = j;
        }

        /* I1/I2: the implicit levels, from the explicit level. */
        for (std::size_t k = 0; k < count; ++k) {
            int lv = base_levels[static_cast<std::size_t>(seq[k])];
            BidiClass const c = st[k];
            if ((lv & 1) == 0) {
                if (c == BidiClass::R) {
                    lv += 1;
                } else if (c == BidiClass::EN || c == BidiClass::AN) {
                    lv += 2;
                }
            } else if (c == BidiClass::L || c == BidiClass::EN ||
                       c == BidiClass::AN) {
                lv += 1;
            }
            levels[static_cast<std::size_t>(seq[k])] = lv;
        }
    }

    /* L1: separators, and the whitespace or isolates before them and at the
     * paragraph's end, take the paragraph level. The sequence is read over the
     * retained characters, because X9's removed controls are gone by now. */
    for (int k = 0; k < m; ++k) {
        int const idx = retained[static_cast<std::size_t>(k)];
        if (types[idx] != BidiClass::B && types[idx] != BidiClass::S) {
            continue;
        }
        levels[idx] = paragraph_level;
        for (int j = k - 1; j >= 0; --j) {
            int const p = retained[static_cast<std::size_t>(j)];
            if (types[p] == BidiClass::WS || is_isolate_or_pdi(types[p])) {
                levels[p] = paragraph_level;
            } else {
                break;
            }
        }
    }
    for (int k = m - 1; k >= 0; --k) {
        int const p = retained[static_cast<std::size_t>(k)];
        if (types[p] == BidiClass::WS || is_isolate_or_pdi(types[p])) {
            levels[p] = paragraph_level;
        } else {
            break;
        }
    }

    /* L2: reorder the retained characters by reversing, from the highest level
     * down to the lowest odd level, every run at or above that level. */
    std::vector<int> order = retained;
    int max_level = 0;
    bool any_odd = false;
    int min_odd = 0;
    for (int idx : order) {
        int const lv = levels[static_cast<std::size_t>(idx)];
        if (lv > max_level) {
            max_level = lv;
        }
        if ((lv & 1) != 0 && (!any_odd || lv < min_odd)) {
            min_odd = lv;
            any_odd = true;
        }
    }
    for (int lev = max_level; lev >= (any_odd ? min_odd : max_level + 1); --lev) {
        for (std::size_t a = 0; a < order.size();) {
            if (levels[static_cast<std::size_t>(order[a])] < lev) {
                ++a;
                continue;
            }
            std::size_t b = a;
            while (b < order.size() &&
                   levels[static_cast<std::size_t>(order[b])] >= lev) {
                ++b;
            }
            std::reverse(order.begin() + static_cast<std::ptrdiff_t>(a),
                         order.begin() + static_cast<std::ptrdiff_t>(b));
            a = b;
        }
    }
    para.order = order;

    for (int i = 0; i < n; ++i) {
        para.levels[static_cast<std::size_t>(i)] =
            static_cast<uint8_t>(levels[i]);
    }
    return para;
}

std::vector<BidiRun> BidiParagraph::runs() const
{
    std::vector<BidiRun> result;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t j = i;
        while (j < text.size() && levels[j] == levels[i]) {
            ++j;
        }
        int const level = levels[i];
        result.push_back({static_cast<int>(i), static_cast<int>(j - i),
                          (level & 1) != 0 ? BidiDirection::RTL : BidiDirection::LTR,
                          level});
        i = j;
    }
    return result;
}

std::vector<int> BidiParagraph::logical_to_visual() const
{
    std::vector<int> mapping(text.size(), -1);
    for (std::size_t v = 0; v < order.size(); ++v) {
        mapping[static_cast<std::size_t>(order[v])] = static_cast<int>(v);
    }
    return mapping;
}

int BidiParagraph::next_cursor_position(int logical_pos, bool forward) const
{
    if (order.empty()) {
        return 0;
    }
    std::vector<int> const mapping = logical_to_visual();
    int visual = 0;
    if (logical_pos >= 0 && static_cast<std::size_t>(logical_pos) < mapping.size()) {
        visual = mapping[static_cast<std::size_t>(logical_pos)];
        if (visual < 0) {
            visual = 0;
        }
    }
    visual += forward ? 1 : -1;
    if (visual < 0) {
        visual = 0;
    }
    if (visual >= static_cast<int>(order.size())) {
        visual = static_cast<int>(order.size()) - 1;
    }
    return order[static_cast<std::size_t>(visual)];
}

std::pair<int, int> BidiParagraph::selection_range(int logical_start,
                                                   int logical_end) const
{
    if (logical_start > logical_end) {
        std::swap(logical_start, logical_end);
    }
    std::vector<int> const mapping = logical_to_visual();
    int v_start = 0;
    int v_end = 0;
    if (logical_start >= 0 && static_cast<std::size_t>(logical_start) < mapping.size()) {
        v_start = mapping[static_cast<std::size_t>(logical_start)];
    }
    if (logical_end >= 0 && static_cast<std::size_t>(logical_end) < mapping.size()) {
        v_end = mapping[static_cast<std::size_t>(logical_end)];
    }
    return {v_start < v_end ? v_start : v_end, v_start < v_end ? v_end : v_start};
}

} // namespace aegir::trinket
