/*
 * The filesystem helper bundle: the header, the table, and the images.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/fsbundle.h>

namespace aegir::fsbundle {

namespace {

uint32_t align_up(uint32_t value) noexcept
{
    return (value + (kAlignment - 1)) & ~(kAlignment - 1);
}

bool name_matches(Entry const &entry, char const *name, uint32_t length) noexcept
{
    uint32_t stored = 0;
    while (stored < kNameMax && entry.name[stored] != '\0') {
        ++stored;
    }
    if (stored != length) {
        return false;
    }
    for (uint32_t i = 0; i < length; ++i) {
        if (entry.name[i] != name[i]) {
            return false;
        }
    }
    return true;
}

/** A hex digit, or -1. */
int hex_digit(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** The canonical text's 32 hex digits, in order, to GPT's mixed-endian
 *  sixteen bytes. False when the text is not a canonical GUID. */
bool guid_to_gpt(char const *text, uint32_t length, uint8_t out[16]) noexcept
{
    if (length != 36) {
        return false;
    }
    uint8_t written[16];
    uint32_t digits = 0;
    for (uint32_t i = 0; i < length; ++i) {
        if (text[i] == '-') {
            /* Hyphens fall at 8, 13, 18, 23 -- but accepting them anywhere
             * would let a malformed text through, so check the positions. */
            if (i != 8 && i != 13 && i != 18 && i != 23) {
                return false;
            }
            continue;
        }
        int const high = hex_digit(text[i]);
        if (high < 0) {
            return false;
        }
        ++i;
        if (i >= length) {
            return false;
        }
        int const low = hex_digit(text[i]);
        if (low < 0 || digits >= 16) {
            return false;
        }
        written[digits++] = static_cast<uint8_t>((high << 4) | low);
    }
    if (digits != 16) {
        return false;
    }
    out[0] = written[3];
    out[1] = written[2];
    out[2] = written[1];
    out[3] = written[0];
    out[4] = written[5];
    out[5] = written[4];
    out[6] = written[7];
    out[7] = written[6];
    for (uint32_t i = 8; i < 16; ++i) {
        out[i] = written[i];
    }
    return true;
}

}  // namespace

uint32_t measure(uint32_t count, uint64_t const *sizes) noexcept
{
    if (count > (0xffffffffu - sizeof(Header)) / sizeof(Entry)) {
        return 0;
    }
    uint64_t total = sizeof(Header) + static_cast<uint64_t>(count) * sizeof(Entry);
    for (uint32_t i = 0; i < count; ++i) {
        total = align_up(static_cast<uint32_t>(total));
        total += sizes[i];
        if (total > 0xffffffffu) {
            return 0;
        }
    }
    return static_cast<uint32_t>(total);
}

uint32_t build(void *out, uint32_t capacity, uint32_t count,
               char const *const *names, uint32_t const *lengths,
               void const *const *images, uint64_t const *sizes) noexcept
{
    if (out == nullptr) {
        return 0;
    }
    uint32_t const needed = measure(count, sizes);
    if (needed == 0 || needed > capacity) {
        return 0;
    }
    auto *bytes = static_cast<uint8_t *>(out);
    auto *header = reinterpret_cast<Header *>(bytes);
    header->magic = kMagic;
    header->count = count;
    auto *entries = reinterpret_cast<Entry *>(bytes + sizeof(Header));
    uint32_t at = sizeof(Header) + count * sizeof(Entry);
    for (uint32_t i = 0; i < count; ++i) {
        if (names[i] == nullptr || images[i] == nullptr || lengths[i] > kNameMax) {
            return 0;
        }
        Entry &entry = entries[i];
        for (uint32_t c = 0; c < kNameMax; ++c) {
            entry.name[c] = c < lengths[i] ? names[i][c] : '\0';
        }
        at = align_up(at);
        entry.offset = at;
        entry.size = static_cast<uint32_t>(sizes[i]);
        /* The compiler's own copy, so the library stays free-standing: it is
         * a builtin, not a libc call. */
        __builtin_memcpy(bytes + at, images[i], static_cast<uint32_t>(sizes[i]));
        at += static_cast<uint32_t>(sizes[i]);
    }
    return at;
}

void const *find(void const *blob, uint32_t bytes, char const *name,
                 uint32_t name_length, uint32_t *size) noexcept
{
    if (blob == nullptr || bytes < sizeof(Header)) {
        return nullptr;
    }
    auto const *header = static_cast<Header const *>(blob);
    if (header->magic != kMagic) {
        return nullptr;
    }
    uint32_t const count = header->count;
    if (count > (bytes - sizeof(Header)) / sizeof(Entry)) {
        return nullptr;
    }
    auto const *entries =
        reinterpret_cast<Entry const *>(static_cast<uint8_t const *>(blob) + sizeof(Header));
    for (uint32_t i = 0; i < count; ++i) {
        Entry const &entry = entries[i];
        if (!name_matches(entry, name, name_length)) {
            continue;
        }
        if (entry.offset > bytes || entry.size > bytes - entry.offset) {
            return nullptr;
        }
        if (size != nullptr) {
            *size = entry.size;
        }
        return static_cast<uint8_t const *>(blob) + entry.offset;
    }
    return nullptr;
}

bool type_matches(Row const &row, uint8_t const gpt_type[16]) noexcept
{
    if (row.type == nullptr) {
        return false;
    }
    uint8_t parsed[16];
    if (!guid_to_gpt(row.type, row.type_length, parsed)) {
        return false;
    }
    for (uint32_t i = 0; i < 16; ++i) {
        if (parsed[i] != gpt_type[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace aegir::fsbundle
