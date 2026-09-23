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
    char name[kMaxName]; /* the inode's own name, from its small data */
    uint32_t name_length;
    uint8_t data[data::kBytes];
};

/** Entry kinds a directory reports, in the volume protocol's numbering. */
constexpr uint64_t kKindFile = 1;
constexpr uint64_t kKindDir = 2;

class Journal;

class Volume {
public:
    /** Read and validate the superblock. False, with the volume invalid, when
     *  the bytes are not a BFS a little-endian reader speaks. `write` is the
     *  write half of the transport; without one the volume is read-only. */
    bool open(ReadSector read, void *context,
              WriteSector write = nullptr) noexcept;

    bool valid() const noexcept { return valid_; }
    bool writable() const noexcept { return write_ != nullptr; }
    bool clean() const noexcept
    {
        return le32(superblock_ + superblock::kFlags) == kClean;
    }
    uint32_t block_size() const noexcept { return 1u << block_shift_; }
    uint64_t num_blocks() const noexcept { return num_blocks_; }
    uint64_t root_block() const noexcept { return to_block(root_); }
    char const *name() const noexcept { return name_; }

    uint32_t block_shift() const noexcept { return block_shift_; }
    uint32_t ag_shift() const noexcept { return ag_shift_; }
    uint32_t num_ags() const noexcept { return num_ags_; }
    uint32_t blocks_per_ag() const noexcept { return blocks_per_ag_; }
    uint64_t used_blocks() const noexcept { return used_blocks_; }

    /** The log region, and its two cursors. The cursors are block positions
     *  within the log run, in [0, log_length()); a clean volume has them
     *  equal (specs/bfs.md's journal). */
    Run log() const noexcept { return log_; }
    uint64_t log_length() const noexcept { return log_.length; }
    uint64_t log_start() const noexcept { return log_start_; }
    uint64_t log_end() const noexcept { return log_end_; }
    bool set_log_start(uint64_t position) noexcept;
    bool set_log_end(uint64_t position) noexcept;

    /** The journal a transaction runs through, or null when writes go
     *  straight to the disk. */
    void attach_journal(Journal *journal) noexcept { journal_ = journal; }
    Journal *journal() const noexcept { return journal_; }

    /** A run's first block number: (group << ag_shift) | start. */
    uint64_t to_block(Run const &run) const noexcept;

    /** The one-block run that names `block`, and whether a run is within the
     *  volume (the journal's replay checks it). */
    Run to_run(uint64_t block) const noexcept;
    bool validate_run(Run const &run) const noexcept;

    /** Whole-block I/O. `write_block` is journalled when a transaction is
     *  open -- it buffers the new image and writes it to the disk at commit
     *  -- and read_block sees those buffered images, so an operation that
     *  writes a block and reads it back within one transaction reads what it
     *  wrote. The `_now` forms always go to the disk: file data, the log
     *  itself and the replay use them. */
    bool read_block(uint64_t block, uint8_t *out) const noexcept;
    bool write_block(uint64_t block, uint8_t const *in) const noexcept;
    bool read_block_now(uint64_t block, uint8_t *out) const noexcept;
    bool write_block_now(uint64_t block, uint8_t const *in) const noexcept;

    /** Patch the in-memory superblock and write its sector: the allocated
     *  count, and the clean/dirty flag. `flush_superblock` is deferred while
     *  a transaction is open -- the commit writes the superblock -- and the
     *  `_now` form always writes it. */
    bool set_used_blocks(uint64_t used) noexcept;
    bool set_flags(uint32_t flags) noexcept;
    bool flush_superblock() const noexcept;
    bool write_superblock_now() const noexcept;

    /** The superblock image, for a transaction to snapshot and restore: an
     *  aborted operation must not leave the in-memory allocated count moved. */
    void save_superblock(uint8_t *out) const noexcept;
    void restore_superblock(uint8_t const *in) noexcept;

    /** Read the inode at `block`. False on a bad magic, a deleted inode, or a
     *  size that does not match the volume's. */
    bool read_inode(uint64_t block, Inode *out) const noexcept;

    /** The next inode at or after `*block`, scanning the volume's blocks.
     *  `*block` is advanced past it; false at the end, with `*block` at the
     *  volume's last block. A query's scan walks the volume this way. */
    bool next_inode(uint64_t *block, Inode *out) const noexcept;

    /** Read `length` bytes of `inode`'s data stream at `offset`, zero-filling
     *  a hole. False when the stream cannot supply them. */
    bool read_stream(Inode const &inode, uint64_t offset, uint8_t *out,
                     uint32_t length) const noexcept;

    /** Write `length` bytes of a stream at `offset`. The stream may spill
     *  into the inode's indirect array, which is read and rewritten here;
     *  the inode block itself is the caller's to write back. False when the
     *  stream cannot cover the range. Journalled, for a stream that is
     *  metadata (a directory's or attribute tree's blocks). */
    bool write_stream_raw(uint8_t const *stream, uint32_t stream_size,
                          uint64_t offset, uint8_t const *in,
                          uint32_t length) const noexcept;

    /** The same write, always to the disk: file data, which was never
     *  journalled (specs/bfs.md's metadata-only log). */
    bool write_stream_direct(uint8_t const *stream, uint32_t stream_size,
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
                    uint32_t length, bool direct) const noexcept;
    bool write_stream_impl(uint8_t const *stream, uint32_t stream_size,
                           uint64_t offset, uint8_t const *in, uint32_t length,
                           bool direct) const noexcept;
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
    Run log_{};
    uint64_t log_start_ = 0;
    uint64_t log_end_ = 0;
    Journal *journal_ = nullptr;
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
