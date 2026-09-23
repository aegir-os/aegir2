/*
 * The Be File System's small_data section: attributes kept in the inode.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/bfs/attribute.h>

namespace aegir::bfs {

namespace {

/* sizeof(small_data): the four-byte type and the two 16-bit sizes. Haiku's
 * small_data::IsLast says an entry at `at` is the terminator once it would not
 * fit the fixed part, so the test is at + 8 > inode_size. */
constexpr uint32_t kFixedPart = 8;

bool in_bounds(uint32_t inode_size, uint32_t at) noexcept
{
    return at + kFixedPart <= inode_size;
}

/* The byte length of the entry at `at`, or 0 when it overruns the inode. The
 * sizes are the manual's; a corrupt pair is refused rather than trusted. */
uint32_t entry_size(uint8_t const *block, uint32_t inode_size,
                    uint32_t at) noexcept
{
    uint32_t const size =
        small_entry_size(le16(block + at + 4), le16(block + at + 6));
    return at + size <= inode_size ? size : 0;
}

bool name_matches(char const *a, uint32_t a_length, char const *b,
                  uint32_t b_length) noexcept
{
    if (a_length != b_length) {
        return false;
    }
    for (uint32_t i = 0; i < a_length; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

bool is_file_name(uint8_t const *block, uint32_t at) noexcept
{
    return le16(block + at + 4) == 1 &&
           static_cast<uint8_t>(block[at + 8]) == kFileNameName;
}

/* Find `name`'s entry and the end of the section. Both the start of the
 * matching entry (0 when absent) and the terminator's offset come back, so a
 * caller that mutates knows what to move and where to append. */
bool locate(uint8_t const *block, uint32_t inode_size, char const *name,
            uint32_t name_length, uint32_t *found_at, uint32_t *end) noexcept
{
    uint32_t at = inode::kSmallData;
    uint32_t found = 0;
    while (in_bounds(inode_size, at) && le16(block + at + 4) != 0) {
        uint32_t const size = entry_size(block, inode_size, at);
        if (size == 0) {
            return false;
        }
        if (!is_file_name(block, at) &&
            name_matches(reinterpret_cast<char const *>(block + at + 8),
                         le16(block + at + 4), name, name_length)) {
            found = at;
        }
        at += size;
    }
    *found_at = found;
    *end = at;
    return true;
}

void zero(uint8_t *block, uint32_t from, uint32_t to) noexcept
{
    for (uint32_t i = from; i < to; ++i) {
        block[i] = 0;
    }
}

}  // namespace

bool small_next(uint8_t const *block, uint32_t inode_size, uint32_t *cursor,
                SmallAttribute *out) noexcept
{
    uint32_t at = *cursor;
    while (in_bounds(inode_size, at) && le16(block + at + 4) != 0) {
        uint32_t const size = entry_size(block, inode_size, at);
        if (size == 0) {
            return false;
        }
        *cursor = at + size;
        if (!is_file_name(block, at)) {
            out->type = le32(block + at);
            out->name = reinterpret_cast<char const *>(block + at + 8);
            out->name_length = le16(block + at + 4);
            out->data = block + at + 8 + out->name_length + 3;
            out->data_length = le16(block + at + 6);
            return true;
        }
        at += size;
    }
    return false;
}

bool small_find(uint8_t const *block, uint32_t inode_size, char const *name,
                uint32_t name_length, SmallAttribute *out) noexcept
{
    uint32_t cursor = inode::kSmallData;
    SmallAttribute entry{};
    while (small_next(block, inode_size, &cursor, &entry)) {
        if (name_matches(entry.name, entry.name_length, name, name_length)) {
            *out = entry;
            return true;
        }
    }
    return false;
}

bool small_set(uint8_t *block, uint32_t inode_size, uint32_t type,
               char const *name, uint32_t name_length, uint8_t const *data,
               uint32_t data_length) noexcept
{
    if (name_length == 0 || name_length > kMaxName) {
        return false;
    }
    uint32_t found_at = 0;
    uint32_t end = 0;
    if (!locate(block, inode_size, name, name_length, &found_at, &end)) {
        return false;
    }
    uint32_t const needed = small_entry_size(name_length, data_length);
    uint32_t const old_size = found_at != 0 ? entry_size(block, inode_size, found_at) : 0;
    if (needed > inode_size - end + old_size) {
        return false;
    }
    if (found_at != 0) {
        /* Close the gap the old entry leaves, then append the new one. */
        for (uint32_t i = 0; i < end - (found_at + old_size); ++i) {
            block[found_at + i] = block[found_at + old_size + i];
        }
        end -= old_size;
    }
    uint8_t *entry = block + end;
    put_le32(entry, type);
    put_le16(entry + 4, static_cast<uint16_t>(name_length));
    put_le16(entry + 6, static_cast<uint16_t>(data_length));
    for (uint32_t i = 0; i < name_length; ++i) {
        entry[8 + i] = static_cast<uint8_t>(name[i]);
    }
    entry[8 + name_length] = 0;
    entry[9 + name_length] = 0;
    entry[10 + name_length] = 0;
    for (uint32_t i = 0; i < data_length; ++i) {
        entry[11 + name_length + i] = data[i];
    }
    entry[11 + name_length + data_length] = 0;
    zero(block, end + needed, inode_size);
    return true;
}

bool small_remove(uint8_t *block, uint32_t inode_size, char const *name,
                  uint32_t name_length) noexcept
{
    uint32_t found_at = 0;
    uint32_t end = 0;
    if (!locate(block, inode_size, name, name_length, &found_at, &end) ||
        found_at == 0) {
        return false;
    }
    uint32_t const size = entry_size(block, inode_size, found_at);
    for (uint32_t i = 0; i < end - (found_at + size); ++i) {
        block[found_at + i] = block[found_at + size + i];
    }
    zero(block, end - size, inode_size);
    return true;
}

bool small_file_name(uint8_t const *block, uint32_t inode_size, char *out,
                     uint32_t capacity, uint32_t *length) noexcept
{
    uint32_t at = inode::kSmallData;
    while (in_bounds(inode_size, at) && le16(block + at + 4) != 0) {
        uint32_t const size = entry_size(block, inode_size, at);
        if (size == 0) {
            return false;
        }
        if (is_file_name(block, at)) {
            uint32_t data_length = le16(block + at + 6);
            uint8_t const *data = block + at + 8 + le16(block + at + 4) + 3;
            /* The stored data may or may not include its terminator. */
            while (data_length != 0 && data[data_length - 1] == 0) {
                --data_length;
            }
            if (data_length > capacity) {
                return false;
            }
            for (uint32_t i = 0; i < data_length; ++i) {
                out[i] = static_cast<char>(data[i]);
            }
            *length = data_length;
            return true;
        }
        at += size;
    }
    return false;
}

}  // namespace aegir::bfs