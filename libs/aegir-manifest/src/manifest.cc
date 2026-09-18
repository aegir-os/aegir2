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
    bool device_manager_seen = false;
    bool device_id_seen = false;
    bool memory_seen = false;
    bool delegate_seen = false;
    bool initrd_seen = false;
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
            /* The seen-flags are per *section*, not per file: every field a section may
             * declare is checked against the section that declared it (and `authority`
             * has been reset here since it was written). Leaving one out means a second
             * section cannot declare it at all, which is what `device_manager` did. */
            authority_seen = false;
            device_manager_seen = false;
            device_id_seen = false;
            memory_seen = false;
            delegate_seen = false;
            initrd_seen = false;
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

        if (equals(key, "device_manager")) {
            if (device_manager_seen) {
                failure_line = number;
                failure = "this key is declared twice in the section";
                return false;
            }
            device_manager_seen = true;
            if (equals(value, "true")) {
                current->device_manager = true;
                return true;
            }
            if (equals(value, "false")) {
                current->device_manager = false;
                return true;
            }
            failure_line = number;
            failure = "device_manager is either `true` or `false`";
            return false;
        }

        if (equals(key, "initrd")) {
            if (initrd_seen) {
                failure_line = number;
                failure = "this key is declared twice in the section";
                return false;
            }
            initrd_seen = true;
            if (equals(value, "true")) {
                current->initrd = true;
                return true;
            }
            if (equals(value, "false")) {
                current->initrd = false;
                return true;
            }
            failure_line = number;
            failure = "initrd is either `true` or `false`";
            return false;
        }

        if (equals(key, "memory_kib")) {
            if (memory_seen) {
                failure_line = number;
                failure = "this key is declared twice in the section";
                return false;
            }
            memory_seen = true;
            /* A decimal number of KiB: what a service asks for is a region to lay its own
             * objects out in, and a page or a few is what that means in practice. The
             * allocator hands out powers of two, so a request that is not one is rounded
             * up to the next -- 5 KiB becomes 8, and the service is given 8. */
            uint32_t kib = 0;
            if (value.length == 0 || value.length > 6) {
                failure_line = number;
                failure = "memory_kib is a decimal number of KiB";
                return false;
            }
            for (uint32_t d = 0; d < value.length; ++d) {
                if (value.data[d] < '0' || value.data[d] > '9') {
                    failure_line = number;
                    failure = "memory_kib is a decimal number of KiB";
                    return false;
                }
                kib = kib * 10 + static_cast<uint32_t>(value.data[d] - '0');
            }
            if (kib == 0) {
                failure_line = number;
                failure = "memory_kib of zero asks for nothing; leave the key out";
                return false;
            }
            uint32_t rounded = 1;
            while (rounded < kib && rounded < (1u << 20)) {
                rounded <<= 1;
            }
            current->memory_kib = rounded;
            return true;
        }

        if (equals(key, "delegate_mib")) {
            if (delegate_seen) {
                failure_line = number;
                failure = "this key is declared twice in the section";
                return false;
            }
            delegate_seen = true;
            /* A decimal number of MiB, for a spawning service's untyped (the key
             * without `spawns` asks for memory nobody can use -- a validation the
             * parser leaves to director, who can see the whole section). Rounded
             * up to a power of two, the way `memory_kib` is. */
            uint32_t mib = 0;
            if (value.length == 0 || value.length > 4) {
                failure_line = number;
                failure = "delegate_mib is a decimal number of MiB";
                return false;
            }
            for (uint32_t d = 0; d < value.length; ++d) {
                if (value.data[d] < '0' || value.data[d] > '9') {
                    failure_line = number;
                    failure = "delegate_mib is a decimal number of MiB";
                    return false;
                }
                mib = mib * 10 + static_cast<uint32_t>(value.data[d] - '0');
            }
            if (mib == 0) {
                failure_line = number;
                failure = "delegate_mib of zero asks for the default; leave the key out";
                return false;
            }
            uint32_t rounded = 1;
            while (rounded < mib && rounded < (1u << 10)) {
                rounded <<= 1;
            }
            current->delegate_mib = rounded;
            return true;
        }

        if (equals(key, "device_id")) {
            if (device_id_seen) {
                failure_line = number;
                failure = "this key is declared twice in the section";
                return false;
            }
            device_id_seen = true;
            /* A small decimal number, which is all a device id is. Read here rather
             * than in a table because the only thing that gives it meaning is the
             * bus, and the bus is not in this file. */
            uint32_t value_number = 0;
            if (value.length == 0 || value.length > 3) {
                failure_line = number;
                failure = "device_id is a small decimal number, or 0 for none";
                return false;
            }
            for (uint32_t d = 0; d < value.length; ++d) {
                if (value.data[d] < '0' || value.data[d] > '9') {
                    failure_line = number;
                    failure = "device_id is a small decimal number, or 0 for none";
                    return false;
                }
                value_number = value_number * 10 + static_cast<uint32_t>(value.data[d] - '0');
            }
            current->device_id = value_number;
            return true;
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
