/*
 * The Be File System's journal -- implementation. See include/aegir/bfs/journal.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The log is a flat stream of entries at the front of the volume. Each entry
 * is a run array -- a block-sized header of `count`, `max_runs`, then the
 * target runs in ascending block order -- followed by one data block per run.
 * Be's replay can only expand runs of length one, so every run here is one
 * block, and the data order matches the array's run order.
 */

#include <aegir/bfs/journal.h>

namespace aegir::bfs {

namespace {

/* The order a run array wants: ascending allocation group, then start. */
bool run_before(Volume const *volume, uint64_t a, uint64_t b) noexcept
{
    Run const left = volume->to_run(a);
    Run const right = volume->to_run(b);
    if (left.allocation_group != right.allocation_group) {
        return left.allocation_group < right.allocation_group;
    }
    return left.start < right.start;
}

}  // namespace

bool Journal::open(Volume *volume) noexcept
{
    if (volume == nullptr || !volume->valid() || !volume->writable()) {
        return false;
    }
    volume_ = volume;
    block_size_ = volume->block_size();
    active_ = false;
    count_ = 0;
    /* A log of one block cannot hold an array and its data. */
    if (volume->log_length() < 2 || block_size_ > kMaxBlockSize) {
        return false;
    }
    volume->attach_journal(this);
    return true;
}

uint64_t Journal::free_blocks() const noexcept
{
    uint64_t const length = volume_->log_length();
    uint64_t const start = volume_->log_start();
    uint64_t const end = volume_->log_end();
    uint64_t const used = (end + length - start) % length;
    return length - used;
}

bool Journal::peek(uint64_t block, uint8_t *out) const noexcept
{
    for (uint32_t i = 0; i < count_; ++i) {
        if (blocks_[i] == block) {
            __builtin_memcpy(out, data_[i], block_size_);
            return true;
        }
    }
    return false;
}

void Journal::begin() noexcept
{
    active_ = true;
    count_ = 0;
    committed_ = false;
    if (volume_ != nullptr) {
        volume_->save_superblock(superblock_);
        snapshot_valid_ = true;
    }
}

bool Journal::log(uint64_t block, uint8_t const *data) noexcept
{
    if (!active_) {
        return false;
    }
    for (uint32_t i = 0; i < count_; ++i) {
        if (blocks_[i] == block) {
            __builtin_memcpy(data_[i], data, block_size_);
            return true;
        }
    }
    if (count_ >= kMaxJournalBlocks) {
        return false;
    }
    blocks_[count_] = block;
    __builtin_memcpy(data_[count_], data, block_size_);
    ++count_;
    return true;
}

void Journal::abort() noexcept
{
    /* Nothing has gone to the disk before the commit's 'DIRT' write, so the
     * in-memory superblock is put back as it was; the buffered blocks are
     * simply dropped. After that write the volume is left dirty for a replay,
     * and the snapshot must not hide it. */
    if (snapshot_valid_ && !committed_ && volume_ != nullptr) {
        volume_->restore_superblock(superblock_);
    }
    snapshot_valid_ = false;
    committed_ = false;
    active_ = false;
    count_ = 0;
}

bool Journal::commit() noexcept
{
    if (!active_) {
        return true;
    }
    active_ = false;
    if (count_ == 0) {
        snapshot_valid_ = false;
        return true;
    }

    uint32_t const runs = count_;
    uint32_t const max_runs = run_array::max_runs(block_size_);
    /* One array holds this transaction: it is bounded well below an array's
     * capacity. The entry is one array block plus one data block per run. */
    if (runs + 1 > free_blocks() || runs > max_runs - 1) {
        count_ = 0;
        return false;
    }

    /* The array's runs ascend; the data follows in that order. */
    uint32_t order[kMaxJournalBlocks];
    for (uint32_t i = 0; i < runs; ++i) {
        order[i] = i;
    }
    for (uint32_t i = 1; i < runs; ++i) {
        uint32_t const key = order[i];
        uint32_t j = i;
        while (j > 0 && run_before(volume_, blocks_[key], blocks_[order[j - 1]])) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = key;
    }

    __builtin_memset(array_, 0, block_size_);
    put_le32(array_ + run_array::kCount, runs);
    put_le32(array_ + run_array::kMaxRuns, max_runs);
    for (uint32_t i = 0; i < runs; ++i) {
        put_run(array_ + run_array::kRuns + i * 8, volume_->to_run(blocks_[order[i]]));
    }

    uint64_t const length = volume_->log_length();
    uint64_t const log_block = volume_->to_block(volume_->log());
    uint64_t const array_position = volume_->log_end();
    if (!volume_->write_block_now(log_block + array_position, array_)) {
        count_ = 0;
        return false;
    }
    uint64_t position = (array_position + 1) % length;
    for (uint32_t i = 0; i < runs; ++i) {
        if (!volume_->write_block_now(log_block + position, data_[order[i]])) {
            count_ = 0;
            return false;
        }
        position = (position + 1) % length;
    }

    /* Commit: the log now covers the transaction. Only after this does any
     * block go to its home. */
    volume_->set_log_end(position);
    volume_->set_flags(kDirty);
    if (!volume_->write_superblock_now()) {
        count_ = 0;
        return false;
    }
    committed_ = true;

    /* Checkpoint: the buffered images go to their homes, then the log is
     * retired. A crash before the retire replays the entry; replaying twice
     * is harmless. */
    for (uint32_t i = 0; i < runs; ++i) {
        if (!volume_->write_block_now(blocks_[i], data_[i])) {
            count_ = 0;
            return false;
        }
    }
    volume_->set_log_start(position);
    volume_->set_flags(kClean);
    count_ = 0;
    committed_ = false;
    snapshot_valid_ = false;
    return volume_->write_superblock_now();
}

bool Journal::replay() noexcept
{
    if (volume_ == nullptr) {
        return false;
    }
    uint64_t start = volume_->log_start();
    uint64_t const end = volume_->log_end();
    if (start == end) {
        return true; /* clean: the fast mount */
    }
    if (!volume_->writable()) {
        return false;
    }

    uint64_t const length = volume_->log_length();
    uint64_t const log_block = volume_->to_block(volume_->log());
    uint32_t const max_runs = run_array::max_runs(block_size_);

    /* A clean log never exceeds its own length in entries; this bounds a
     * corrupt one. */
    for (uint64_t walked = 0; walked < length; ++walked) {
        if (!volume_->read_block_now(log_block + start, array_)) {
            return false;
        }
        uint32_t const count = le32(array_ + run_array::kCount);
        uint32_t const stored_max = le32(array_ + run_array::kMaxRuns);
        if (stored_max != max_runs || count == 0 || count > max_runs - 1) {
            return false;
        }
        uint64_t position = (start + 1) % length;
        for (uint32_t i = 0; i < count; ++i) {
            Run const run = le_run(array_ + run_array::kRuns + i * 8);
            if (!volume_->validate_run(run)) {
                return false;
            }
        }
        for (uint32_t i = 0; i < count; ++i) {
            Run const run = le_run(array_ + run_array::kRuns + i * 8);
            if (!volume_->read_block_now(log_block + position, data_[0]) ||
                !volume_->write_block_now(volume_->to_block(run), data_[0])) {
                return false;
            }
            position = (position + 1) % length;
        }
        start = position;
        if (start == end) {
            break;
        }
    }

    volume_->set_log_start(end);
    volume_->set_flags(kClean);
    return volume_->write_superblock_now();
}

}  // namespace aegir::bfs