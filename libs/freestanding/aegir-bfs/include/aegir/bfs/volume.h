/*
 * Reading a Be File System volume.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The read half of specs/bfs.md: open a volume by its superblock, read an
 * inode, read a file's or a directory's data through its extent tree, and walk
 * a directory's B+tree. Nothing here writes, and nothing here knows how a
 * block is fetched -- the service hands in a read callback, so the same code
 * serves the freestanding service and a host test.
 *
 * The volume holds its own block-sized buffers, because a stream read is a
 * sequence of blocks and the caller's buffer is not where the block lands.
 * They are members rather than locals so a service reading on a small stack
 * does not spend it here.
 */

#ifndef AEGIR_BFS_VOLUME_H
#define AEGIR_BFS_VOLUME_H

#include <aegir/bfs/layout.h>
#include <stdint.h>

namespace aegir::bfs {

/** Read the volume-relative 512-byte sector `sector` into `out`. False when
 *  the device refuses. The callback is the service's: sectors arrive through
 *  its window and its partition range. A sector, not a block, because the
 *  block size lives in the superblock the first sectors carry. */
using ReadSector = bool (*)(void *context, uint64_t sector, uint8_t *out);

/** Write the 512 bytes of `in` to the volume-relative sector `sector`. The
 *  write half of the same transport; null on a volume opened read-only. */
using WriteSector = bool (*)(void *context, uint64_t sector, uint8_t const *in);

/** What a caller needs of an inode: its kind, its size, its time, its place in
 *  the tree, and its data stream (so a stream read needs no second visit). */
struct Inode {
    uint32_t mode;
    uint32_t type; /* an attribute inode's type_code; zero otherwise */
    int64_t size;
    int64_t mtime;
    Run run;
    Run parent;
    Run attributes; /* the attribute directory, or a zero run for none */
    uint8_t data[data::kBytes];
};

/** Entry kinds a directory reports, in the volume protocol's numbering. */
constexpr uint64_t kKindFile = 1;
constexpr uint64_t kKindDir = 2;

class Volume {
public:
    /** Read and validate the superblock. False, with the volume invalid, when
     *  the bytes are not a BFS a little-endian reader speaks. `write` is the
     *  write half of the transport; without one the volume is read-only. */
    bool open(ReadSector read, void *context,
              WriteSector write = nullptr) noexcept;

    bool valid() const noexcept { return valid_; }
    bool writable() const noexcept { return write_ != nullptr; }
    uint32_t block_size() const noexcept { return 1u << block_shift_; }
    uint64_t num_blocks() const noexcept { return num_blocks_; }
    uint64_t root_block() const noexcept { return to_block(root_); }
    char const *name() const noexcept { return name_; }

    uint32_t block_shift() const noexcept { return block_shift_; }
    uint32_t ag_shift() const noexcept { return ag_shift_; }
    uint32_t num_ags() const noexcept { return num_ags_; }
    uint32_t blocks_per_ag() const noexcept { return blocks_per_ag_; }
    uint64_t used_blocks() const noexcept { return used_blocks_; }

    /** A run's first block number: (group << ag_shift) | start. */
    uint64_t to_block(Run const &run) const noexcept;

    /** Whole-block I/O. Read is a sequence of sectors; write its other
     *  direction. The allocate-and-write side of the library needs both. */
    bool read_block(uint64_t block, uint8_t *out) const noexcept;
    bool write_block(uint64_t block, uint8_t const *in) const noexcept;

    /** Patch the in-memory superblock and write its sector: the allocated
     *  count, and the clean/dirty flag. A clean volume has log_start ==
     *  log_end and stays that way in this phase (specs/bfs.md). */
    bool set_used_blocks(uint64_t used) noexcept;
    bool set_flags(uint32_t flags) noexcept;
    bool flush_superblock() const noexcept;

    /** Read the inode at `block`. False on a bad magic, a deleted inode, or a
     *  size that does not match the volume's. */
    bool read_inode(uint64_t block, Inode *out) const noexcept;

    /** Read `length` bytes of `inode`'s data stream at `offset`, zero-filling
     *  a hole. False when the stream cannot supply them. */
    bool read_stream(Inode const &inode, uint64_t offset, uint8_t *out,
                     uint32_t length) const noexcept;

    /** Write `length` bytes of a stream at `offset`. The stream may spill
     *  into the inode's indirect array, which is read and rewritten here;
     *  the inode block itself is the caller's to write back. False when the
     *  stream cannot cover the range. */
    bool write_stream_raw(uint8_t const *stream, uint32_t stream_size,
                          uint64_t offset, uint8_t const *in,
                          uint32_t length) const noexcept;

    /** Find `name` in the directory `dir`'s tree. False when it is not there. */
    bool dir_find(Inode const &dir, char const *name, uint32_t length,
                  uint64_t *inode_block) const noexcept;

    /** The `index`th entry of `dir`, or false past the last. The cursor is the
     *  caller's index, as the volume protocol's list wants. A directory too
     *  large for a single leaf node is refused, not misread -- the growth
     *  phase brings the cursor that walks internal nodes. */
    bool dir_entry(Inode const &dir, uint32_t index, char *name,
                   uint32_t *name_length, uint64_t *inode_block) const noexcept;

    /** Read an attribute's type and size. An attribute lives in the inode's
     *  small_data section or in an inode under the attribute directory; both
     *  are searched. False when the inode has no such attribute. */
    bool attr_stat(Inode const &inode, char const *name, uint32_t name_length,
                   uint32_t *type, uint64_t *size) const noexcept;

    /** Read the attribute's bytes at `offset`. `*length` is how many bytes the
     *  caller wants and, on return, how many were read. False when the
     *  attribute is not there. */
    bool attr_read(Inode const &inode, char const *name, uint32_t name_length,
                   uint64_t offset, uint8_t *out,
                   uint32_t *length) const noexcept;

    /** The `index`th attribute of `inode`: small data first, then the
     *  attribute directory. False past the last. */
    bool attr_entry(Inode const &inode, uint32_t index, char *name,
                    uint32_t *name_length, uint32_t *type,
                    uint64_t *size) const noexcept;

    /** The attribute inode's block when `name` lives in the attribute
     *  directory; false when it is small data or absent. */
    bool attr_inode(Inode const &inode, char const *name, uint32_t name_length,
                    uint64_t *attr_block) const noexcept;

private:
    uint64_t run_bytes(Run const &run) const noexcept;
    bool read_part(Run const &run, uint64_t skip, uint8_t *out,
                   uint32_t length) const noexcept;
    bool write_part(Run const &run, uint64_t skip, uint8_t const *in,
                    uint32_t length) const noexcept;
    bool node_header(Inode const &dir, uint32_t *node_size, uint64_t *root,
                     uint64_t *maximum) const noexcept;
    bool attr_dir_inode(Inode const &inode, Inode *dir) const noexcept;
    bool inode_raw(Inode const &inode, uint32_t *inode_size) const noexcept;
    uint32_t node_key_lengths(uint8_t const *node, uint16_t count,
                              uint16_t *lengths) const noexcept;
    bool node_key(uint8_t const *node, uint16_t count, uint16_t index, char *out,
                  uint32_t *length) const noexcept;

    ReadSector read_ = nullptr;
    WriteSector write_ = nullptr;
    void *context_ = nullptr;
    uint32_t block_shift_ = 0;
    uint32_t ag_shift_ = 0;
    uint32_t num_ags_ = 0;
    uint32_t blocks_per_ag_ = 0;
    uint64_t num_blocks_ = 0;
    uint64_t used_blocks_ = 0;
    Run root_{};
    Run indices_{};
    bool valid_ = false;
    char name_[32] = {};
    uint8_t superblock_[kSuperblockBytes] = {};

    mutable uint8_t block_[kMaxBlockSize];
    mutable uint8_t array_[kMaxBlockSize];
    mutable uint8_t array2_[kMaxBlockSize];
    mutable uint8_t tree_[kMaxBlockSize];
};

/** A path component matched as BFS matches names: bytewise, exactly. */
bool name_equals(char const *a, uint32_t a_length, char const *b,
                 uint32_t b_length) noexcept;

}  // namespace aegir::bfs

#endif  // AEGIR_BFS_VOLUME_H
