/*
 * The Be File System's small_data section: attributes kept in the inode.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * An inode's bytes after the fixed part hold zero or more small_data entries
 * until a terminator. Each is an eight-byte header (type, name_size,
 * data_size), the name, three pad bytes, the data, and a terminator; the data
 * begins at the name's start plus name_size plus 3, exactly as Haiku's
 * small_data::Data() computes it.
 *
 * The first entry an Aegir inode carries is the reserved file name: type
 * 'CSTR', a one-byte name 0x13, and the name as its data. Every walk here
 * skips it; it is not an attribute a client may see or change.
 *
 * These are the pure byte operations -- no allocation, no volume. The Writer
 * above them decides whether an attribute fits in the inode or needs an
 * attribute inode (specs/bfs.md).
 */

#ifndef AEGIR_BFS_ATTRIBUTE_H
#define AEGIR_BFS_ATTRIBUTE_H

#include <aegir/bfs/layout.h>
#include <stdint.h>

namespace aegir::bfs {

/** One small_data entry, with its name and data pointing into the inode
 *  block the walk was given. */
struct SmallAttribute {
    uint32_t type;
    char const *name;
    uint32_t name_length;
    uint8_t const *data;
    uint32_t data_length;
};

/** The bytes one entry needs: the eight-byte header, the name, three pad
 *  bytes, the data, and a terminator. */
inline uint32_t small_entry_size(uint32_t name_length,
                                 uint32_t data_length) noexcept
{
    return 12 + name_length + data_length;
}

/** Walk `block`'s small_data section from `*cursor`; true with `*out` filled
 *  for the next attribute, false at the terminator. Start `*cursor` at
 *  inode::kSmallData. The reserved file-name entry is skipped. */
bool small_next(uint8_t const *block, uint32_t inode_size, uint32_t *cursor,
                SmallAttribute *out) noexcept;

/** Find the attribute `name`; false when it is not in the small_data
 *  section. */
bool small_find(uint8_t const *block, uint32_t inode_size, char const *name,
                uint32_t name_length, SmallAttribute *out) noexcept;

/** Put the attribute, replacing one of the same name. False when it does not
 *  fit, in which case the block is unchanged. */
bool small_set(uint8_t *block, uint32_t inode_size, uint32_t type,
               char const *name, uint32_t name_length, uint8_t const *data,
               uint32_t data_length) noexcept;

/** Drop the attribute. False when it was not there. */
bool small_remove(uint8_t *block, uint32_t inode_size, char const *name,
                  uint32_t name_length) noexcept;

/** The reserved file-name entry's data: the inode's own name, NUL-terminated
 *  as stored. False when the inode has none. */
bool small_file_name(uint8_t const *block, uint32_t inode_size, char *out,
                     uint32_t capacity, uint32_t *length) noexcept;

}  // namespace aegir::bfs

#endif  // AEGIR_BFS_ATTRIBUTE_H