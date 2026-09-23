/*
 * Writing a Be File System volume.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The write half of specs/bfs.md: making and removing files and directories,
 * writing a file's data into its extent tree, cutting a file down (or growing
 * it), and reading a directory's B+tree back to change it. It owns a block
 * allocator and all the scratch buffers the operations need, so the service
 * above it holds no format state at all.
 *
 * A directory's entries live in one leaf node here. A directory that outgrows
 * its node -- tens of entries -- is refused rather than miswritten; the node
 * split and the internal cursor are the next step in this phase. The same is
 * true of a stream that outgrows the indirect runs: a run is added to the
 * direct runs, then to the indirect array, and a stream that needs the double
 * indirect is refused, not corrupted.
 */

#ifndef AEGIR_BFS_WRITER_H
#define AEGIR_BFS_WRITER_H

#include <aegir/bfs/allocator.h>
#include <aegir/bfs/layout.h>
#include <aegir/bfs/volume.h>
#include <stdint.h>

namespace aegir::bfs {

class Writer {
public:
    /** Bind to a writable, valid volume. */
    bool open(Volume *volume) noexcept;

    /** Make a file (`mode` a regular file's) or a directory under the
     *  directory inode at `parent_block`, named `name`, stamped `time`. A
     *  directory is born with its dot and dotdot. The new inode's block is
     *  `*out_block`. False on a full volume, a bad name, or a parent whose
     *  tree is full. */
    bool create(uint64_t parent_block, char const *name, uint32_t name_length,
                uint32_t mode, int64_t time, uint64_t *out_block) noexcept;

    /** Remove the entry `name` from the directory at `parent_block` and free
     *  what it named -- its data runs and its inode. A directory that is not
     *  empty refuses. */
    bool remove(uint64_t parent_block, char const *name,
                uint32_t name_length) noexcept;

    /** Rename `from` to `to`, both in the directory at `parent_block`. The
     *  inode and its data do not move; the destination must not exist. */
    bool rename(uint64_t parent_block, char const *from, uint32_t from_length,
                char const *to, uint32_t to_length) noexcept;

    /** Write `length` bytes at logical `offset` in the stream of the inode at
     *  `inode_block`, growing the stream with zeroed blocks as needed. */
    bool write(uint64_t inode_block, uint64_t offset, uint8_t const *bytes,
               uint32_t length, int64_t time) noexcept;

    /** Resize the inode's stream to `size`: free the tail it no longer needs,
     *  or grow it with zeroed blocks. */
    bool truncate(uint64_t inode_block, uint64_t size, int64_t time) noexcept;

    /** Free the inode at `block`: its stream's runs, then the block itself.
     *  The directory entry has to be gone already. */
    bool destroy(uint64_t block) noexcept;

private:
    bool read_inode_block(uint64_t block, uint8_t *out) noexcept;
    bool tree_header(uint64_t parent_block, uint8_t *stream, uint32_t *node_size,
                     uint64_t *root, uint64_t *maximum) noexcept;
    bool tree_edit(uint64_t parent_block, char const *name, uint32_t name_length,
                   uint64_t value, bool insert, bool *existed) noexcept;
    bool dir_is_empty(uint64_t dir_block) noexcept;

    bool append_run(uint8_t *stream, Run const &run) noexcept;
    uint64_t stream_blocks(uint8_t const *stream) const noexcept;
    bool grow_to(uint8_t *stream, uint64_t needed_blocks,
                 uint64_t *covered) noexcept;
    bool zero_range(uint8_t *stream, uint64_t from, uint64_t to) noexcept;
    bool free_stream(uint8_t const *stream) noexcept;
    bool trim_stream(uint8_t *stream, uint64_t new_blocks) noexcept;

    Volume *volume_ = nullptr;
    Allocator allocator_;

    uint8_t inode_[kMaxBlockSize] = {};
    uint8_t stream_[data::kBytes] = {};
    mutable uint8_t node_[kMaxBlockSize] = {};
    uint8_t work_[kMaxBlockSize] = {};
    uint8_t zero_[kMaxBlockSize] = {};
};

}  // namespace aegir::bfs

#endif  // AEGIR_BFS_WRITER_H