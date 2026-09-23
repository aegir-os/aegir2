/*
 * Building and patching a Be File System inode block.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The writer makes inodes and changes them; the reader parses them. This is
 * the writer's half: a fresh inode's fixed fields and its reserved file-name
 * small data, and the data stream, size and time patched back in. The bytes
 * are the manual's offsets (layout.h); nothing is written by casting a struct
 * over the block.
 */

#ifndef AEGIR_BFS_INODE_H
#define AEGIR_BFS_INODE_H

#include <aegir/bfs/layout.h>
#include <stdint.h>

namespace aegir::bfs {

/** Fill `block` with a fresh, empty inode: in use, one block, its own run as
 *  its inode number, `parent`, `mode`, `time` for both stamps, an empty data
 *  stream, and the reserved file-name small data carrying `name`. */
void inode_build(uint8_t *block, uint32_t block_size, Run const &run,
                 Run const &parent, uint32_t mode, int64_t time, char const *name,
                 uint32_t name_length) noexcept;

/** Copy the data stream (direct, indirect, double, max ranges, size) out of an
 *  inode block. */
void inode_get_stream(uint8_t const *block, uint8_t *stream) noexcept;

/** Patch the data stream, the logical size and the last-modified time into an
 *  inode block, leaving every other field alone. */
void inode_set_stream(uint8_t *block, uint8_t const *stream, int64_t size,
                      int64_t mtime) noexcept;

/** Mark the inode deleted and not in use, so a block reused as something else
 *  cannot be mistaken for it. */
void inode_mark_deleted(uint8_t *block) noexcept;

inline uint32_t inode_mode(uint8_t const *block) noexcept
{
    return le32(block + inode::kMode);
}

inline int64_t inode_size(uint8_t const *block) noexcept
{
    return le64_signed(block + inode::kData + data::kSize);
}

inline int64_t inode_mtime(uint8_t const *block) noexcept
{
    return le64_signed(block + inode::kLastModified);
}

/** True when the mode is a directory (S_IFDIR). */
inline bool mode_is_directory(uint32_t mode) noexcept
{
    return (mode & kModeTypeMask) == kModeDirectory;
}

}  // namespace aegir::bfs

#endif  // AEGIR_BFS_INODE_H