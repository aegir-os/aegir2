/*
 * Trinket Unicode utilities.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_UNICODE_H
#define AEGIR_TRINKET_UNICODE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aegir::trinket {

// UTF-8 <-> UTF-32 conversion
std::u32string utf8_to_utf32(std::string_view utf8);
std::string utf32_to_utf8(std::u32string_view utf32);

// Codepoint iteration. The iterator points at the next codepoint to read:
// `operator*` decodes it and `operator++` steps past it. It must not decode
// and advance together -- the range-for's end-check runs after `++`, so a
// step that both loads the last codepoint and moves to the end drops it.
struct Utf8Iterator {
    const char* ptr = nullptr;
    const char* end = nullptr;

    Utf8Iterator() = default;
    Utf8Iterator(std::string_view sv) : ptr(sv.data()), end(sv.data() + sv.size()) {}

    /* The end sentinel: the same end pointer. A default-built iterator cannot
     * serve -- its `ptr` is indeterminate, so `!=` never becomes false. */
    static Utf8Iterator end_of(std::string_view sv) {
        Utf8Iterator it;
        it.ptr = sv.data() + sv.size();
        it.end = it.ptr;
        return it;
    }

    bool operator!=(const Utf8Iterator& other) const { return ptr != other.ptr; }
    uint32_t operator*() const { return decode(); }
    Utf8Iterator& operator++() { ptr += width(); return *this; }

private:
    uint32_t decode() const {
        if (ptr >= end) return 0;
        unsigned char const c = static_cast<unsigned char>(*ptr);
        if (c < 0x80) return c;
        if ((c & 0xE0) == 0xC0 && ptr + 1 < end) {
            return ((c & 0x1F) << 6) | (ptr[1] & 0x3F);
        }
        if ((c & 0xF0) == 0xE0 && ptr + 2 < end) {
            return ((c & 0x0F) << 12) | ((ptr[1] & 0x3F) << 6) | (ptr[2] & 0x3F);
        }
        if ((c & 0xF8) == 0xF0 && ptr + 3 < end) {
            return ((c & 0x07) << 18) | ((ptr[1] & 0x3F) << 12) |
                   ((ptr[2] & 0x3F) << 6) | (ptr[3] & 0x3F);
        }
        return 0xFFFD;
    }

    size_t width() const {
        if (ptr >= end) return 1;
        unsigned char const c = static_cast<unsigned char>(*ptr);
        if (c < 0x80) return 1;
        if ((c & 0xE0) == 0xC0 && ptr + 1 < end) return 2;
        if ((c & 0xF0) == 0xE0 && ptr + 2 < end) return 3;
        if ((c & 0xF8) == 0xF0 && ptr + 3 < end) return 4;
        return 1;  // an invalid lead advances one byte
    }
};

struct Utf8Range {
    std::string_view sv;
    Utf8Iterator begin() const { return Utf8Iterator(sv); }
    Utf8Iterator end() const { return Utf8Iterator::end_of(sv); }
};

inline Utf8Range utf8_iterate(std::string_view sv) { return Utf8Range{sv}; }

// Validation
bool is_valid_utf8(std::string_view sv);
std::string replace_invalid_utf8(std::string_view sv, char32_t replacement = 0xFFFD);

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_UNICODE_H