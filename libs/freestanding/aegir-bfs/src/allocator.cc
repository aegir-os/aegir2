/*
 * The Be File System's block allocator.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/bfs/allocator.h>

namespace aegir::bfs {

bool Allocator::open(Volume *volume) noexcept
{
    if (volume == nullptr || !volume->valid() || !volume->writable()) {
        return false;
    }
    volume_ = volume;
    block_size_ = volume->block_size();
    blocks_per_ag_ = volume->blocks_per_ag();
    ag_shift_ = volume->ag_shift();
    return true;
}

uint64_t Allocator::group_bits(uint32_t group) const noexcept
{
    uint64_t const first = static_cast<uint64_t>(group) << ag_shift_;
    uint64_t const span = 1ull << ag_shift_;
    uint64_t const total = volume_->num_blocks();
    if (first >= total) {
        return 0;
    }
    return span < total - first ? span : total - first;
}

uint64_t Allocator::bitmap_block(uint32_t group, uint32_t index) const noexcept
{
    return 1 + static_cast<uint64_t>(group) * blocks_per_ag_ + index;
}

bool Allocator::bit_set(uint8_t const *bitmap, uint32_t bit) const noexcept
{
    return (bitmap[(bit / 32) * 4 + (bit % 32) / 8] >> (bit % 8)) & 1u;
}

void Allocator::set_bit(uint8_t *bitmap, uint32_t bit) noexcept
{
    bitmap[(bit / 32) * 4 + (bit % 32) / 8] |= static_cast<uint8_t>(1u << (bit % 8));
}

void Allocator::clear_bit(uint8_t *bitmap, uint32_t bit) noexcept
{
    bitmap[(bit / 32) * 4 + (bit % 32) / 8] &=
        static_cast<uint8_t>(~(1u << (bit % 8)));
}

bool Allocator::allocate(uint32_t max_blocks, Run *out,
                         uint32_t min_blocks) noexcept
{
    if (volume_ == nullptr || out == nullptr || max_blocks == 0 ||
        min_blocks == 0) {
        return false;
    }
    if (max_blocks > kMaxRun) {
        max_blocks = kMaxRun;
    }
    if (min_blocks > max_blocks) {
        min_blocks = max_blocks;
    }
    uint32_t const per_block = bits_per_block();
    for (uint32_t group = 0; group < volume_->num_ags(); ++group) {
        uint64_t const bits = group_bits(group);
        if (bits == 0) {
            break;
        }
        for (uint32_t index = 0; index < blocks_per_ag_; ++index) {
            uint64_t const at = bitmap_block(group, index);
            if (at >= volume_->num_blocks() || !volume_->read_block(at, bitmap_)) {
                return false;
            }
            uint32_t const base = index * per_block;
            for (uint32_t bit = 0; bit < per_block; ++bit) {
                uint32_t const local = base + bit;
                if (local >= bits) {
                    break;
                }
                if (bit_set(bitmap_, bit)) {
                    continue;
                }
                /* The free block at `local` starts a run; extend it while
                 * blocks are free and the run stays inside this bitmap
                 * block, so the one block can be written back. */
                uint32_t length = 1;
                while (length < max_blocks && local + length < bits &&
                       (local + length) / per_block == index &&
                       !bit_set(bitmap_, local + length)) {
                    ++length;
                }
                if (length < min_blocks) {
                    continue;
                }
                for (uint32_t k = 0; k < length; ++k) {
                    set_bit(bitmap_, local + k);
                }
                if (!volume_->write_block(at, bitmap_)) {
                    return false;
                }
                *out = run_make(group, static_cast<uint16_t>(local),
                                static_cast<uint16_t>(length));
                volume_->set_used_blocks(volume_->used_blocks() + length);
                (void)volume_->flush_superblock();
                return true;
            }
        }
    }
    return false;
}

bool Allocator::free(Run const &run) noexcept
{
    if (volume_ == nullptr || run.length == 0) {
        return false;
    }
    uint32_t const per_block = bits_per_block();
    for (uint32_t k = 0; k < run.length; ++k) {
        uint32_t const local = run.start + k;
        uint32_t const index = local / per_block;
        uint32_t const bit = local % per_block;
        uint64_t const at = bitmap_block(run.allocation_group, index);
        if (at >= volume_->num_blocks() || !volume_->read_block(at, bitmap_)) {
            return false;
        }
        clear_bit(bitmap_, bit);
        if (!volume_->write_block(at, bitmap_)) {
            return false;
        }
    }
    uint64_t const used = volume_->used_blocks();
    volume_->set_used_blocks(used >= run.length ? used - run.length : 0);
    (void)volume_->flush_superblock();
    return true;
}

}  // namespace aegir::bfs