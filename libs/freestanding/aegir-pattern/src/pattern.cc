/*
 * AmigaDOS wildcards -- implementation (see the header and specs/pattern.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A recursive descent with backtracking, the shape AmigaDOS's own MatchPattern
 * is: a sub-pattern span [pi, pe) matches a name span [ni, ne) exactly. `#` and
 * `~` are the tokens that try more than one length, and each recurses on the
 * rest; every other token consumes at most one character.
 */

#include <aegir/pattern.h>

namespace aegir::pattern {

namespace {

/* AmigaDOS filenames are case-insensitive; the matcher folds ASCII case the
 * way the filesystems do. */
char fold(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/* The index past the expression that starts at `at`: one character, `?`, `%`,
 * a quoted character, a `[...]` class, a `(...)` group, or a `#`/`~` applied to
 * one of those. A malformed tail runs to `pe`. */
uint32_t expr_end(char const *p, uint32_t pe, uint32_t at) noexcept {
    if (at >= pe) {
        return at;
    }
    switch (p[at]) {
    case '\'':
        return at + 2 <= pe ? at + 2 : pe;
    case '[': {
        uint32_t i = at + 1;
        while (i < pe && p[i] != ']') {
            ++i;
        }
        return i < pe ? i + 1 : pe;
    }
    case '(': {
        int depth = 0;
        for (uint32_t i = at; i < pe; ++i) {
            if (p[i] == '(') {
                ++depth;
            } else if (p[i] == ')') {
                if (--depth == 0) {
                    return i + 1;
                }
            }
        }
        return pe;
    }
    case '#':
    case '~':
        return expr_end(p, pe, at + 1);
    default:
        return at + 1;
    }
}

/* The index of the `|` or the `)` that ends the group alternative starting at
 * `at`; the group's closing `)` is at `ge - 1`. */
uint32_t alternative_end(char const *p, uint32_t ge, uint32_t at) noexcept {
    int depth = 0;
    uint32_t i = at;
    while (i + 1 < ge) {
        if (p[i] == '(') {
            ++depth;
        } else if (p[i] == ')') {
            --depth;
        } else if (p[i] == '|' && depth == 0) {
            break;
        }
        ++i;
    }
    return i;
}

/* True when `ch` is in the class [at, ce); `ce - 1` is the closing `]`. */
bool class_has(char const *p, uint32_t at, uint32_t ce, char ch) noexcept {
    uint32_t i = at + 1;
    bool negated = false;
    if (i < ce - 1 && p[i] == '~') {
        negated = true;
        ++i;
    }
    bool found = false;
    while (i < ce - 1) {
        if (i + 2 < ce - 1 && p[i + 1] == '-') {
            if (fold(ch) >= fold(p[i]) && fold(ch) <= fold(p[i + 2])) {
                found = true;
            }
            i += 3;
        } else {
            if (fold(ch) == fold(p[i])) {
                found = true;
            }
            ++i;
        }
    }
    return found != negated;
}

bool match_sub(char const *p, uint32_t pi, uint32_t pe,
               char const *s, uint32_t ni, uint32_t ne) noexcept;

/* Zero or more of the expression [xp, xe), then the rest [rp, pe). One
 * repetition consumes at least one character, so a nullable expression cannot
 * loop; the zero repetition is the `match_sub` of the rest below. */
bool match_rep(char const *p, uint32_t xp, uint32_t xe, uint32_t rp, uint32_t pe,
               char const *s, uint32_t ni, uint32_t ne) noexcept {
    if (match_sub(p, rp, pe, s, ni, ne)) {
        return true;
    }
    for (uint32_t mid = ni + 1; mid <= ne; ++mid) {
        if (match_sub(p, xp, xe, s, ni, mid) &&
            match_rep(p, xp, xe, rp, pe, s, mid, ne)) {
            return true;
        }
    }
    return false;
}

/* The sub-pattern [pi, pe) matches s[ni, ne) exactly. */
bool match_sub(char const *p, uint32_t pi, uint32_t pe,
               char const *s, uint32_t ni, uint32_t ne) noexcept {
    if (pi >= pe) {
        return ni >= ne;
    }
    switch (p[pi]) {
    case '#': {
        uint32_t const xe = expr_end(p, pe, pi + 1);
        return match_rep(p, pi + 1, xe, xe, pe, s, ni, ne);
    }
    case '~': {
        /* ~X consumes a run that X does not match, then the rest must match.
         * Every length is tried, so ~(foo) takes the whole name and ~x? takes
         * one non-x character. */
        uint32_t const xe = expr_end(p, pe, pi + 1);
        for (uint32_t mid = ni; mid <= ne; ++mid) {
            if (!match_sub(p, pi + 1, xe, s, ni, mid) &&
                match_sub(p, xe, pe, s, mid, ne)) {
                return true;
            }
        }
        return false;
    }
    case '(': {
        uint32_t const ge = expr_end(p, pe, pi);
        if (ge < pi + 2 || p[ge - 1] != ')') {
            return false;  /* an unterminated group matches nothing */
        }
        for (uint32_t a = pi + 1; a + 1 < ge;) {
            uint32_t const ae = alternative_end(p, ge, a);
            for (uint32_t mid = ni; mid <= ne; ++mid) {
                if (match_sub(p, a, ae, s, ni, mid) &&
                    match_sub(p, ge, pe, s, mid, ne)) {
                    return true;
                }
            }
            a = ae + 1;
        }
        return false;
    }
    case '[': {
        uint32_t const ce = expr_end(p, pe, pi);
        if (ce < pi + 2 || p[ce - 1] != ']') {
            return false;  /* an unterminated class matches nothing */
        }
        return ni < ne && class_has(p, pi, ce, s[ni]) &&
               match_sub(p, ce, pe, s, ni + 1, ne);
    }
    case '\'':
        return pi + 1 < pe && ni < ne && fold(p[pi + 1]) == fold(s[ni]) &&
               match_sub(p, pi + 2, pe, s, ni + 1, ne);
    case '%':
        return match_sub(p, pi + 1, pe, s, ni, ne);
    case '?':
        return ni < ne && match_sub(p, pi + 1, pe, s, ni + 1, ne);
    default:
        return ni < ne && fold(p[pi]) == fold(s[ni]) &&
               match_sub(p, pi + 1, pe, s, ni + 1, ne);
    }
}

}  // namespace

bool match(char const *pattern, uint32_t pattern_length,
           char const *name, uint32_t name_length) noexcept {
    if (pattern == nullptr || name == nullptr) {
        return false;
    }
    return match_sub(pattern, 0, pattern_length, name, 0, name_length);
}

}  // namespace aegir::pattern
