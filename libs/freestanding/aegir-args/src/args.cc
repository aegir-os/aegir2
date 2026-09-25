/*
 * aegir::args: the Amiga's ReadArgs -- implementation. See args.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * No allocation, no libc++, no seL4: the template and argv are the caller's,
 * and a lookup re-walks the line rather than keeping a table. A template item
 * is a name then a run of flags after '/'; the items are split on ','. A line
 * is read item by item: NAME=VALUE names a value outright, a bare name is a
 * switch or a keyword (whose value is the next token), and anything else fills
 * the next positional item in template order. A /M positional takes the bare
 * tokens but leaves one for each positional item after it, as ReadArgs does,
 * so `FROM/M TO/A` can reach TO.
 */

#include "aegir/args.h"

namespace aegir::args {

namespace {

bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }

char lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

uint32_t text_length(char const *text) noexcept
{
    uint32_t n = 0;
    while (text != nullptr && text[n] != '\0') {
        ++n;
    }
    return n;
}

bool eq_ci(char const *a, uint32_t alen, char const *b, uint32_t blen) noexcept
{
    if (alen != blen) {
        return false;
    }
    for (uint32_t i = 0; i < alen; ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

int32_t find_char(char const *text, uint32_t length, char wanted) noexcept
{
    for (uint32_t i = 0; i < length; ++i) {
        if (text[i] == wanted) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

bool is_number(char const *text, uint32_t length) noexcept
{
    uint32_t i = 0;
    if (i < length && (text[i] == '-' || text[i] == '+')) {
        ++i;
    }
    if (i == length) {
        return false;
    }
    for (; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

/* One template item: its name is a slice of the template, not NUL-terminated. */
struct Item {
    char const *name;
    uint32_t name_length;
    bool required;
    bool switch_;
    bool keyword;
    bool multiple;
    bool number;
};

/* The index-th comma-separated item of the template. False past the end. */
bool item_at(char const *tmpl, uint32_t index, Item &out) noexcept
{
    uint32_t at = 0;
    uint32_t current = 0;
    for (;;) {
        while (is_space(tmpl[at])) {
            ++at;
        }
        char const *name = tmpl + at;
        uint32_t name_length = 0;
        while (tmpl[at] != '\0' && tmpl[at] != ',' && tmpl[at] != '/') {
            ++at;
            ++name_length;
        }
        while (name_length > 0 && is_space(name[name_length - 1])) {
            --name_length;
        }
        Item item{name, name_length, false, false, false, false, false};
        if (tmpl[at] == '/') {
            ++at;
            while (tmpl[at] != '\0' && !is_space(tmpl[at]) && tmpl[at] != ',') {
                switch (lower(tmpl[at])) {
                case 'a': item.required = true; break;
                case 's': item.switch_ = true; break;
                case 'k': item.keyword = true; break;
                case 'm': item.multiple = true; break;
                case 'n': item.number = true; break;
                default: break;
                }
                ++at;
            }
        }
        while (tmpl[at] != '\0' && tmpl[at] != ',') {
            ++at;
        }
        if (current == index) {
            out = item;
            return name_length != 0;
        }
        if (tmpl[at] == '\0') {
            return false;
        }
        ++at; /* past the comma */
        ++current;
    }
}

bool item_by_name(char const *tmpl, char const *name, uint32_t length,
                  Item &out) noexcept
{
    for (uint32_t i = 0;; ++i) {
        Item item{};
        if (!item_at(tmpl, i, item)) {
            return false;
        }
        if (eq_ci(item.name, item.name_length, name, length)) {
            out = item;
            return true;
        }
    }
}

/* The index-th item that takes a positional value: not a switch, not a
 * keyword. This is the order a bare token fills them in. */
bool positional_item_at(char const *tmpl, uint32_t index, Item &out) noexcept
{
    uint32_t seen = 0;
    for (uint32_t i = 0;; ++i) {
        Item item{};
        if (!item_at(tmpl, i, item)) {
            return false;
        }
        if (item.switch_ || item.keyword) {
            continue;
        }
        if (seen == index) {
            out = item;
            return true;
        }
        ++seen;
    }
}

/* How many positional items the template has: the ones a bare token fills. */
uint32_t positional_count(char const *tmpl) noexcept
{
    uint32_t count = 0;
    for (uint32_t i = 0;; ++i) {
        Item item{};
        if (!item_at(tmpl, i, item)) {
            return count;
        }
        if (!item.switch_ && !item.keyword) {
            ++count;
        }
    }
}

/* How many bare positional tokens remain in argv from `start`. A token that
 * names a switch takes one slot, a keyword takes two (its name and the value
 * after it), and a NAME=VALUE names its item outright; anything else is a
 * positional. This is what a /M item reserves for the items after it. */
uint32_t positional_tokens_after(char const *tmpl, char const *const *argv,
                                 int argc, int start) noexcept
{
    uint32_t count = 0;
    for (int i = start; i < argc; ++i) {
        char const *const token = argv[i];
        uint32_t const length = text_length(token);
        if (find_char(token, length, '=') >= 0) {
            continue; /* NAME=VALUE fills its item by name, not in order */
        }
        Item item{};
        if (item_by_name(tmpl, token, length, item)) {
            if (item.switch_) {
                continue;
            }
            if (item.keyword) {
                ++i; /* its value, not a positional */
                continue;
            }
        }
        ++count;
    }
    return count;
}

/* One supplied value: the template item it filled, the value (a slice of argv),
 * and whether the line named an item the template has. */
struct Supplied {
    Item item;
    char const *value;
    uint32_t value_length;
    bool known;
};

/* Walk the line once, in the order the values were written. */
template <typename Visit>
void for_each(Result const &r, Visit visit) noexcept
{
    uint32_t positional = 0;
    int i = 0;
    while (i < r.argc) {
        char const *token = r.argv[i];
        uint32_t const token_length = text_length(token);
        int32_t const eq = find_char(token, token_length, '=');
        if (eq >= 0) {
            Item item{};
            if (item_by_name(r.tmpl, token, static_cast<uint32_t>(eq), item)) {
                char const *const value = token + eq + 1;
                visit(Supplied{item, value, token_length - (eq + 1), true});
            } else {
                visit(Supplied{Item{}, token, token_length, false});
            }
        } else {
            Item item{};
            /* A bare token names a switch or a keyword; a positional item's
             * name is not a keyword -- `copy a b` fills FROM then TO, and
             * `TO` on its own is a value, not the name of one. (To name a
             * positional exactly, write NAME=VALUE.) */
            if (item_by_name(r.tmpl, token, token_length, item) &&
                (item.switch_ || item.keyword)) {
                if (item.switch_) {
                    visit(Supplied{item, "", 0, true});
                } else {
                    char const *value = "";
                    uint32_t value_length = 0;
                    if (i + 1 < r.argc) {
                        value = r.argv[i + 1];
                        value_length = text_length(value);
                        ++i;
                    }
                    visit(Supplied{item, value, value_length, true});
                }
            } else {
                Item place{};
                if (positional_item_at(r.tmpl, positional, place)) {
                    visit(Supplied{place, token, token_length, true});
                    /* A /M item takes the bare tokens, but it must leave one
                     * for each positional item after it -- otherwise a later
                     * positional, `Copy FROM/M TO/A`'s TO, could never be
                     * reached. It stops when no more than that many positional
                     * tokens remain; a plain item advances at once. */
                    if (!place.multiple) {
                        ++positional;
                    } else {
                        uint32_t const rest = positional_count(r.tmpl) - positional - 1;
                        if (positional_tokens_after(r.tmpl, r.argv, r.argc, i + 1) <= rest) {
                            ++positional;
                        }
                    }
                } else {
                    visit(Supplied{Item{}, token, token_length, false});
                }
            }
        }
        ++i;
    }
}

template <typename Match>
bool first_value_with(Result const &r, char const *name, uint32_t length,
                      Match match) noexcept
{
    bool found = false;
    for_each(r, [&](Supplied const &s) {
        if (!found && s.known &&
            eq_ci(s.item.name, s.item.name_length, name, length)) {
            match(s);
            found = true;
        }
    });
    return found;
}

template <typename Match>
bool first_value(Result const &r, char const *name, Match match) noexcept
{
    return first_value_with(r, name, text_length(name), match);
}

}  // namespace

Result read(char const *tmpl, int argc, char const *const *argv) noexcept
{
    Result r{tmpl, argc, argv, "", nullptr, 0};
    for_each(r, [&](Supplied const &s) {
        if (r.problem[0] != '\0') {
            return;
        }
        if (!s.known) {
            r.problem = "unknown argument";
            r.missing = s.value;
            r.missing_length = s.value_length;
            return;
        }
        if (s.item.number && !is_number(s.value, s.value_length)) {
            r.problem = "not a number";
            r.missing = s.item.name;
            r.missing_length = s.item.name_length;
        }
    });
    if (r.problem[0] != '\0') {
        return r;
    }

    /* Every /A item must have been supplied. */
    for (uint32_t i = 0;; ++i) {
        Item item{};
        if (!item_at(tmpl, i, item)) {
            break;
        }
        if (!item.required) {
            continue;
        }
        bool const found =
            first_value_with(r, item.name, item.name_length, [](Supplied const &) {});
        if (!found) {
            r.problem = "a required argument is missing";
            r.missing = item.name;
            r.missing_length = item.name_length;
            return r;
        }
    }
    return r;
}

char const *Result::value(char const *name) const noexcept
{
    char const *answer = nullptr;
    first_value(*this, name, [&](Supplied const &s) { answer = s.value; });
    return answer;
}

bool Result::present(char const *name) const noexcept
{
    return first_value(*this, name, [](Supplied const &) {});
}

uint32_t Result::count(char const *name) const noexcept
{
    uint32_t const length = text_length(name);
    uint32_t occurrences = 0;
    for_each(*this, [&](Supplied const &s) {
        if (s.known && eq_ci(s.item.name, s.item.name_length, name, length)) {
            ++occurrences;
        }
    });
    return occurrences;
}

char const *Result::at(char const *name, uint32_t index) const noexcept
{
    uint32_t const length = text_length(name);
    uint32_t seen = 0;
    char const *answer = nullptr;
    for_each(*this, [&](Supplied const &s) {
        if (answer == nullptr && s.known &&
            eq_ci(s.item.name, s.item.name_length, name, length)) {
            if (seen == index) {
                answer = s.value;
            }
            ++seen;
        }
    });
    return answer;
}

}  // namespace aegir::args
