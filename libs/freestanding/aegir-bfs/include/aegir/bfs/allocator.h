/*
 * The Be File System's block allocator.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Blocks are allocated out of allocation groups, which are numbered from
 * zero; a run never crosses a group. The bitmap for group i lives in
 * `blocks_per_ag` blocks starting at block `1 + i * blocks_per_ag` (block 0 is
 * the superblock and the reserved area: the bitmap starts directly after it,
 * Haiku's BlockAllocator::InitializeAndClearBitmap). Bit `b` of a little-endian
 * 32-bit chunk `b / 32` marks group-local block `b`.
 *
 * The allocator keeps no bitmap in memory: it reads the block that holds the
 * bit, changes it, and writes it back. The volume is small and the calls are
 * few, and a bitmap held in RAM would be one more thing to keep in step with
 * the disk. The superblock's used_blocks is updated and flushed with every
 * allocation and free, which is also where the clean/dirty flag lives.
 */

#ifndef AEGIR_BFS_ALLOCATOR_H
#define AEGIR_BFS_ALLOCATOR_H

#include <aegir/bfs/layout.h>
#include <aegir/bfs/volume.h>
#include <stdint.h>

namespace aegir::bfs {

class Allocator {
public:
    /** Bind to a writable volume. False when the volume is not one. */
    bool open(Volume *volume) noexcept;

    /** Allocate one run of up to `max_blocks` contiguous free blocks, at most
     *  65535 (a run's length field), and at least `min_blocks`. False when no
     *  run that long is free. */
    bool allocate(uint32_t max_blocks, Run *out,
                  uint32_t min_blocks = 1) noexcept;

    /** Return a run's blocks to the free pool. */
    bool free(Run const &run) noexcept;

private:
    static constexpr uint32_t kMaxRun = 65535;

    uint32_t bits_per_block() const noexcept { return block_size_ * 8; }
    uint64_t group_bits(uint32_t group) const noexcept;
    uint64_t bitmap_block(uint32_t group, uint32_t index) const noexcept;
    bool bit_set(uint8_t const *bitmap, uint32_t bit) const noexcept;
    void set_bit(uint8_t *bitmap, uint32_t bit) noexcept;
    void clear_bit(uint8_t *bitmap, uint32_t bit) noexcept;

    Volume *volume_ = nullptr;
    uint32_t block_size_ = 0;
    uint32_t blocks_per_ag_ = 1;
    uint32_t ag_shift_ = 0;
    uint8_t bitmap_[kMaxBlockSize] = {};
};

}  // namespace aegir::bfs

#endif  // AEGIR_BFS_ALLOCATOR_H