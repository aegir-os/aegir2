/*
 * Resource limits, configuration -- implementation. See include/aegir/limits.h
 * and specs/limits.md for the format, the subjects and the precedence.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One line walker serves both entry points: parse validates and keeps the
 * view, resolve scans it again for the most specific rule. There is no rule
 * storage, so there is no capacity to guess and nothing to free -- the file is
 * small and a service resolves its users once at boot.
 */

#include <aegir/limits.h>

namespace aegir::limits {

namespace {

bool is_space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r';
}

void trim(char const *&begin, char const *&end) noexcept
{
    while (begin < end && is_space(*begin)) {
        ++begin;
    }
    while (end > begin && is_space(*(end - 1))) {
        --end;
    }
}

View view(char const *begin, char const *end) noexcept
{
    return View{begin, static_cast<uint32_t>(end - begin)};
}

bool same(View left, View right) noexcept
{
    if (left.length != right.length) {
        return false;
    }
    for (uint32_t i = 0; i < left.length; ++i) {
        if (left.data[i] != right.data[i]) {
            return false;
        }
    }
    return true;
}

/** Compare a view with a NUL-terminated literal: the whole view must be it. */
bool equals(View value, char const *text) noexcept
{
    if (value.data == nullptr || text == nullptr) {
        return false;
    }
    uint32_t i = 0;
    for (; i < value.length; ++i) {
        if (text[i] == '\0' || value.data[i] != text[i]) {
            return false;
        }
    }
    return text[i] == '\0';
}

bool starts_with(View value, char const *prefix, uint32_t prefix_length) noexcept
{
    if (value.data == nullptr || value.length <= prefix_length) {
        return false;
    }
    for (uint32_t i = 0; i < prefix_length; ++i) {
        if (value.data[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

/* "[default]", "[class.name]" or "[user.name]" -> the subject it opens. The
 * brackets must be the whole (trimmed) line, and a class or user needs a name. */
struct Subject {
    enum class Kind : uint8_t { Default, Class, User };
    Kind kind;
    View name;
};

bool section_subject(char const *begin, char const *end, Subject *out) noexcept
{
    if (begin >= end || *begin != '[' || *(end - 1) != ']') {
        return false;
    }
    char const *inner = begin + 1;
    char const *inner_end = end - 1;
    trim(inner, inner_end);
    View name = view(inner, inner_end);
    if (equals(name, "default")) {
        out->kind = Subject::Kind::Default;
        out->name = View{nullptr, 0};
        return true;
    }
    if (starts_with(name, "class.", 6)) {
        out->kind = Subject::Kind::Class;
        out->name = view(inner + 6, inner_end);
        return true;
    }
    if (starts_with(name, "user.", 5)) {
        out->kind = Subject::Kind::User;
        out->name = view(inner + 5, inner_end);
        return true;
    }
    return false;
}

/** The end of the line, and whether a newline was there. By hand so this
 *  library needs no C library symbols (manifest.cc's reason). */
char const *find_end(char const *begin, char const *limit, bool &has_newline) noexcept
{
    char const *cursor = begin;
    while (cursor < limit && *cursor != '\n') {
        ++cursor;
    }
    has_newline = cursor < limit;
    return cursor;
}

/* "key = value", the key up to the first '=' and the value the trimmed rest. */
bool split(char const *begin, char const *end, View &key, View &value) noexcept
{
    char const *equals_at = begin;
    while (equals_at < end && *equals_at != '=') {
        ++equals_at;
    }
    if (equals_at == end) {
        return false;
    }
    char const *key_end = equals_at;
    char const *value_begin = equals_at + 1;
    trim(begin, key_end);
    trim(value_begin, end);
    if (begin == key_end) {
        return false;
    }
    key = view(begin, key_end);
    value = view(value_begin, end);
    return true;
}

/* The two keys the first cut knows. The resource and action are named
 * explicitly rather than split on the underscore, so an unknown key is one
 * clear refusal instead of a half-read rule. */
bool known_key(View key, Resource *resource, Action *action) noexcept
{
    if (equals(key, "memory_log")) {
        *resource = Resource::Memory;
        *action = Action::Log;
        return true;
    }
    if (equals(key, "memory_deny")) {
        *resource = Resource::Memory;
        *action = Action::Deny;
        return true;
    }
    return false;
}

/* Bytes with an optional K, M or G suffix, binary (1024). False on an empty or
 * malformed value, a suffix this version does not know, or an overflow. */
bool parse_amount(View value, uint64_t *out) noexcept
{
    if (value.data == nullptr || value.length == 0) {
        return false;
    }
    uint64_t amount = 0;
    uint32_t at = 0;
    for (; at < value.length; ++at) {
        char const digit = value.data[at];
        if (digit < '0' || digit > '9') {
            break;
        }
        uint64_t const d = static_cast<uint64_t>(digit - '0');
        if (amount > (UINT64_MAX - d) / 10) {
            return false;
        }
        amount = amount * 10 + d;
    }
    if (at == 0) {
        return false;
    }
    uint64_t multiplier = 1;
    if (at < value.length) {
        switch (value.data[at]) {
        case 'K':
            multiplier = 1ull << 10;
            break;
        case 'M':
            multiplier = 1ull << 20;
            break;
        case 'G':
            multiplier = 1ull << 30;
            break;
        default:
            return false;
        }
        ++at;
    }
    if (at != value.length || amount > UINT64_MAX / multiplier) {
        return false;
    }
    *out = amount * multiplier;
    return true;
}

/* Walk the lines, calling visit(subject, resource, action, amount) for every
 * well-formed rule. False, with `problem` filled, on the first bad line. */
template <typename Visit>
bool walk(char const *text, uint32_t length, Limits::Problem *problem, Visit visit) noexcept
{
    char const *line = text;
    char const *const limit = text + length;
    uint32_t number = 1;
    Subject subject{Subject::Kind::Default, View{nullptr, 0}};
    bool have_section = false;
    /* Per section: a bit per action, so a repeated key in one section is the
     * typo it is rather than a silent last-one-wins. */
    uint32_t seen = 0;
    while (line < limit) {
        bool has_newline = false;
        char const *const end = find_end(line, limit, has_newline);
        char const *begin = line;
        char const *trimmed_end = end;
        trim(begin, trimmed_end);
        if (begin == trimmed_end || *begin == '#') {
            line = has_newline ? end + 1 : limit;
            ++number;
            continue;
        }
        if (*begin == '[') {
            if (!section_subject(begin, trimmed_end, &subject)) {
                problem->line = number;
                problem->message =
                    "a section is [default], [class.name] or [user.name]";
                return false;
            }
            have_section = true;
            seen = 0;
        } else {
            if (!have_section) {
                problem->line = number;
                problem->message = "a key must come after a [section]";
                return false;
            }
            View key;
            View value;
            if (!split(begin, trimmed_end, key, value)) {
                problem->line = number;
                problem->message = "expected key = value, or a [section]";
                return false;
            }
            Resource resource = Resource::Memory;
            Action action = Action::Log;
            if (!known_key(key, &resource, &action)) {
                problem->line = number;
                problem->message = "unknown key";
                return false;
            }
            uint32_t const bit = static_cast<uint32_t>(action);
            if ((seen & (1u << bit)) != 0) {
                problem->line = number;
                problem->message = "this key is declared twice in the section";
                return false;
            }
            seen |= 1u << bit;
            uint64_t amount = 0;
            if (!parse_amount(value, &amount)) {
                problem->line = number;
                problem->message = "an amount is bytes with an optional K, M or G";
                return false;
            }
            visit(subject, resource, action, amount);
        }
        line = has_newline ? end + 1 : limit;
        ++number;
    }
    return true;
}

}  // namespace

bool Limits::parse(char const *text, uint32_t length) noexcept
{
    problem_ = Limits::Problem{0, "no problem"};
    valid_ = false;
    if (text == nullptr || length == 0) {
        problem_ = Limits::Problem{1, "the limits file is empty"};
        return false;
    }
    Problem problem{0, "no problem"};
    bool const ok = walk(text, length, &problem,
                         [](Subject const &, Resource, Action, uint64_t) noexcept {});
    if (!ok) {
        problem_ = problem;
        return false;
    }
    text_ = text;
    length_ = length;
    valid_ = true;
    return true;
}

Amount Limits::resolve(View user, View klass, Resource resource, Action action) const noexcept
{
    Amount best{false, 0};
    if (!valid_ || text_ == nullptr) {
        return best;
    }
    uint8_t best_score = 0;
    /* The file validated at parse, so the visitor cannot see a bad line; the
     * problem out-parameter is still required by the walker's shape. */
    Problem ignored{0, "no problem"};
    (void)walk(text_, length_, &ignored,
               [&](Subject const &subject, Resource rule_resource, Action rule_action,
                   uint64_t amount) noexcept {
                   if (rule_resource != resource || rule_action != action) {
                       return;
                   }
                   uint8_t score = 0;
                   if (subject.kind == Subject::Kind::User && same(subject.name, user)) {
                       score = kPrecedenceUser;
                   } else if (subject.kind == Subject::Kind::Class &&
                              same(subject.name, klass)) {
                       score = kPrecedenceClass;
                   } else if (subject.kind == Subject::Kind::Default) {
                       score = kPrecedenceDefault;
                   }
                   if (score > best_score) {
                       best_score = score;
                       best = Amount{true, amount};
                   }
               });
    return best;
}

}  // namespace aegir::limits
