/*
 * The boot manifest -- implementation. See include/aegir/manifest.h and
 * specs/services.md for the format and the rules it enforces.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/manifest.h>

namespace aegir::manifest {

bool equals(View view, char const *text) noexcept
{
    if (view.data == nullptr || text == nullptr) {
        return false;
    }
    uint32_t i = 0;
    for (; i < view.length; ++i) {
        if (text[i] == '\0' || view.data[i] != text[i]) {
            return false;
        }
    }
    return text[i] == '\0';
}

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

bool empty(View value) noexcept
{
    return value.data == nullptr;
}

/* "key = value", with the key up to the first '=' and the value as the rest. */
bool split(char const *begin, char const *end, View &key, View &value) noexcept
{
    char const *equals = begin;
    while (equals < end && *equals != '=') {
        ++equals;
    }
    if (equals == end) {
        return false;
    }
    char const *key_end = equals;
    char const *value_begin = equals + 1;
    trim(begin, key_end);
    trim(value_begin, end);
    if (begin == key_end) {
        return false;
    }
    key = view(begin, key_end);
    value = view(value_begin, end);
    return true;
}

bool is_section_header(char const *begin, char const *end) noexcept
{
    return begin < end && *begin == '[';
}

/* "[name]" -> name, or false when the brackets are not the whole line. */
bool section_name(char const *begin, char const *end, View &name) noexcept
{
    if (!is_section_header(begin, end) || *(end - 1) != ']') {
        return false;
    }
    char const *inner_begin = begin + 1;
    char const *inner_end = end - 1;
    trim(inner_begin, inner_end);
    if (inner_begin == inner_end) {
        return false;
    }
    name = view(inner_begin, inner_end);
    return true;
}

/* The end of the line starting at `begin`, and whether a newline was there.
 * Done by hand so this library needs no C library symbols: musl's string.h
 * collides with GCC's C++ builtins (see libs/aegir-mem/src/arena.cc). */
char const *find_end(char const *begin, char const *limit, bool &has_newline) noexcept
{
    char const *cursor = begin;
    while (cursor < limit && *cursor != '\n') {
        ++cursor;
    }
    has_newline = cursor < limit;
    return cursor;
}

/* Walk the lines of the manifest, calling `visit(begin, end, number)`. */
template <typename Visit>
bool for_each_line(char const *text, uint32_t length, Visit visit) noexcept
{
    char const *line = text;
    char const *const limit = text + length;
    uint32_t number = 1;
    while (line < limit) {
        bool has_newline = false;
        char const *const end = find_end(line, limit, has_newline);
        if (!visit(line, end, number)) {
            return false;
        }
        line = has_newline ? end + 1 : limit;
        ++number;
    }
    return true;
}

}  // namespace

Manifest::Manifest(mem::Arena &arena, mem::Account &account) noexcept
    : arena_(arena), account_(account), entries_(nullptr), count_(0), capacity_(0),
      problem_{0, "no problem"}
{
}

bool Manifest::fail(uint32_t line, char const *message) noexcept
{
    problem_.line = line;
    problem_.message = message;
    return false;
}

Entry *Manifest::find(View name) noexcept
{
    for (uint32_t i = 0; i < count_; ++i) {
        if (same(entries_[i].name, name)) {
            return &entries_[i];
        }
    }
    return nullptr;
}

bool Manifest::parse(char const *text, uint32_t length) noexcept
{
    if (text == nullptr || length == 0) {
        return fail(1, "the manifest is empty");
    }

    /* First pass: how many sections there are. The entry array is sized from the
     * text itself rather than from a maximum guessed here, and is charged to the
     * account it is allocated for (specs/authority.md). */
    uint32_t sections = 0;
    bool counted = for_each_line(text, length, [&sections](char const *begin, char const *end,
                                                           uint32_t) noexcept {
        trim(begin, end);
        if (is_section_header(begin, end)) {
            ++sections;
        }
        return true;
    });
    if (!counted) {
        return fail(1, "the manifest could not be read");
    }

    capacity_ = sections;
    if (sections > 0) {
        auto *storage = static_cast<Entry *>(arena_.allocate(sizeof(Entry) * sections));
        if (storage == nullptr) {
            return fail(1, "no memory for the manifest's entries");
        }
        account_.slots += sections;
        entries_ = storage;
    }

    /* Second pass: fill them in. */
    bool format_seen = false;
    Entry *current = nullptr;
    bool authority_seen = false;
    uint32_t failure_line = 1;
    char const *failure = nullptr;

    bool parsed = for_each_line(text, length, [&](char const *begin, char const *end,
                                                 uint32_t number) noexcept {
        trim(begin, end);
        if (begin == end || *begin == '#') {
            return true;
        }

        if (is_section_header(begin, end)) {
            View name;
            if (!section_name(begin, end, name)) {
                failure_line = number;
                failure = "a section header must be [name]";
                return false;
            }
            if (!format_seen) {
                failure_line = number;
                failure = "`format` must come before the first section";
                return false;
            }
            if (find(name) != nullptr) {
                failure_line = number;
                failure = "this section appears twice";
                return false;
            }
            if (count_ >= capacity_) {
                failure_line = number;
                failure = "more sections were found than the first pass saw";
                return false;
            }
            current = &entries_[count_++];
            *current = Entry{};
            current->name = name;
            current->line = number;
            current->authority = Authority::System;
            authority_seen = false;
            return true;
        }

        View key;
        View value;
        if (!split(begin, end, key, value)) {
            failure_line = number;
            failure = "expected `key = value`, or a [section]";
            return false;
        }

        if (current == nullptr) {
            if (!equals(key, "format")) {
                failure_line = number;
                failure = "only `format` may come before the first section";
                return false;
            }
            if (!equals(value, "1")) {
                failure_line = number;
                failure = "this is not a manifest version this director understands";
                return false;
            }
            format_seen = true;
            return true;
        }

        struct Binding {
            char const *name;
            View Entry::*field;
        };
        static constexpr Binding kViewKeys[] = {
            {"binary", &Entry::binary},   {"account", &Entry::account},
            {"owns", &Entry::owns},       {"needs", &Entry::needs},
            {"grants", &Entry::grants},   {"spawns", &Entry::spawns},
            {"restart", &Entry::restart},   {"priority", &Entry::priority},
            {"args", &Entry::args},
        };
        for (Binding const &binding : kViewKeys) {
            if (equals(key, binding.name)) {
                if (!empty(current->*(binding.field))) {
                    failure_line = number;
                    failure = "this key is declared twice in the section";
                    return false;
                }
                current->*(binding.field) = value;
                return true;
            }
        }

        if (equals(key, "authority")) {
            if (authority_seen) {
                failure_line = number;
                failure = "this key is declared twice in the section";
                return false;
            }
            authority_seen = true;
            current->authority_text = value;
            if (equals(value, "system")) {
                current->authority = Authority::System;
                return true;
            }
            if (equals(value, "user")) {
                current->authority = Authority::User;
                return true;
            }
            failure_line = number;
            failure = "authority is either `system` or `user`";
            return false;
        }

        failure_line = number;
        failure = "unknown key";
        return false;
    });

    if (!parsed) {
        return fail(failure_line, failure);
    }
    if (!format_seen) {
        return fail(1, "`format` is missing");
    }

    /* Required fields, per specs/services.md. */
    for (uint32_t i = 0; i < count_; ++i) {
        Entry const &entry = entries_[i];
        if (empty(entry.binary)) {
            problem_.line = entry.line;
            problem_.message = "this service has no `binary`";
            return false;
        }
        if (empty(entry.account)) {
            problem_.line = entry.line;
            problem_.message = "this service has no `account`";
            return false;
        }
        if (empty(entry.authority_text)) {
            problem_.line = entry.line;
            problem_.message = "this service has no `authority`";
            return false;
        }
    }
    return true;
}

}  // namespace aegir::manifest
