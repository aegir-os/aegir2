/*
 * The Be File System's journal: a redo log of metadata blocks.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * BFS is a journaling filesystem (specs/bfs.md). A transaction collects the
 * metadata blocks it changed, and at commit writes them to the log -- a run
 * array listing the target blocks in ascending order, then one data block for
 * each -- before writing them to their homes. A crash between the two is
 * repaired by replaying the log on mount: the superblock says 'DIRT' and
 * log_start != log_end, so replay copies each logged block back to its target,
 * sets log_start = log_end and the volume 'CLEN'. A clean volume has
 * log_start == log_end, so its mount replays nothing -- the clean fast mount.
 *
 * The log is the metadata-only log BeOS chose: file data is never journalled,
 * so a file written when the machine dies may lose its tail, but the
 * filesystem's own structures -- inodes, directory and attribute trees, the
 * allocation bitmap, the superblock -- are always consistent.
 *
 * Because an operation reads back blocks it wrote earlier in the same
 * transaction (a run inserted into an indirect array is read again to place
 * the next run), the transaction buffers the blocks it changed and the Volume
 * reads through the buffer until commit. A transaction is bounded by
 * kMaxJournalBlocks; a metadata operation that needs more is refused rather
 * than committed in pieces.
 */

#ifndef AEGIR_BFS_JOURNAL_H
#define AEGIR_BFS_JOURNAL_H

#include <aegir/bfs/layout.h>
#include <aegir/bfs/volume.h>
#include <stdint.h>

namespace aegir::bfs {

/** The most blocks one transaction may change. Metadata transactions are a
 *  handful; the log itself holds hundreds, so this bound is the operation's,
 *  not the disk's. */
constexpr uint32_t kMaxJournalBlocks = 16;

class Journal {
public:
    /** Read the log run. False when the volume has no usable log. */
    bool open(Volume *volume) noexcept;

    /** Replay any pending entries, leaving the volume clean. Called before
     *  the volume is served, so a mount after a crash repairs itself. */
    bool replay() noexcept;

    bool active() const noexcept { return active_; }

    /** Buffer a block's new image for the open transaction. False when the
     *  transaction is full; the caller must not proceed. */
    void begin() noexcept;
    bool log(uint64_t block, uint8_t const *data) noexcept;
    bool commit() noexcept;
    void abort() noexcept;

    /** The buffered image of `block`, when the open transaction changed it.
     *  The Volume reads through this so an operation sees its own writes. */
    bool peek(uint64_t block, uint8_t *out) const noexcept;

private:
    uint64_t free_blocks() const noexcept;

    /* No initializers on the big buffers: a global Journal would otherwise
     * carry them in the image's data section. `open` puts the scalars in a
     * known state, and every buffer is written before it is read. */
    Volume *volume_ = nullptr;
    uint32_t block_size_ = 0;
    bool active_ = false;
    uint32_t count_ = 0;
    /* The superblock as it was when the transaction began, so an aborted
     * operation leaves the in-memory allocated count as it found it. Once the
     * commit has written the 'DIRT' superblock the snapshot is dropped: a
     * failure after that leaves a log a replay will retire. */
    bool snapshot_valid_ = false;
    bool committed_ = false;
    uint8_t superblock_[kSuperblockBytes];
    uint64_t blocks_[kMaxJournalBlocks];
    uint8_t data_[kMaxJournalBlocks][kMaxBlockSize];
    uint8_t array_[kMaxBlockSize];
};

}  // namespace aegir::bfs

#endif  // AEGIR_BFS_JOURNAL_H