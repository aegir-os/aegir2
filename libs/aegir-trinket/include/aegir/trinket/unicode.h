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

// Codepoint iteration
struct Utf8Iterator {
    const char* ptr;
    const char* end;
    uint32_t codepoint = 0;

    Utf8Iterator() = default;
    Utf8Iterator(std::string_view sv) : ptr(sv.data()), end(sv.data() + sv.size()) { next(); }

    bool operator!=(const Utf8Iterator& other) const { return ptr != other.ptr; }
    uint32_t operator*() const { return codepoint; }
    Utf8Iterator& operator++() { next(); return *this; }

private:
    void next() {
        if (ptr >= end) { codepoint = 0; return; }
        unsigned char c = *ptr;
        if (c < 0x80) { codepoint = c; ptr += 1; }
        else if ((c & 0xE0) == 0xC0 && ptr + 1 < end) {
            codepoint = ((c & 0x1F) << 6) | (ptr[1] & 0x3F); ptr += 2;
        }
        else if ((c & 0xF0) == 0xE0 && ptr + 2 < end) {
            codepoint = ((c & 0x0F) << 12) | ((ptr[1] & 0x3F) << 6) | (ptr[2] & 0x3F); ptr += 3;
        }
        else if (ptr + 3 < end) {
            codepoint = ((c & 0x07) << 18) | ((ptr[1] & 0x3F) << 12) |
                        ((ptr[2] & 0x3F) << 6) | (ptr[3] & 0x3F); ptr += 4;
        }
        else { codepoint = 0xFFFD; ptr = end; }
    }
};

struct Utf8Range {
    std::string_view sv;
    Utf8Iterator begin() const { return Utf8Iterator(sv); }
    Utf8Iterator end() const { return Utf8Iterator(); }
};

inline Utf8Range utf8_iterate(std::string_view sv) { return Utf8Range{sv}; }

// Validation
bool is_valid_utf8(std::string_view sv);
std::string replace_invalid_utf8(std::string_view sv, char32_t replacement = 0xFFFD);

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_UNICODE_H