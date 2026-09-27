/*
 * A shared capability-slot pool (specs/memory.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One contiguous CSpace range whose slots are owned and freed by name, so an
 * allocator that carves and frees untypeds over its life (the memory service)
 * reuses slots instead of leaking them past a bump cursor. The owner table is
 * the caller's, its length the range's, so the pool carries no capacity.
 */

#ifndef AEGIR_MEM_SLOT_POOL_H
#define AEGIR_MEM_SLOT_POOL_H

#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::mem {

class SlotPool {
public:
    /** Adopt the range [first, first + count) and read/write `owners`, a
     *  caller array of `count` words (zero is free). */
    void adopt(seL4_CPtr first, seL4_Word count, uint32_t *owners) noexcept
    {
        first_ = first;
        count_ = count;
        owners_ = owners;
        for (seL4_Word i = 0; i < count_; ++i) {
            owners_[i] = 0;
        }
    }

    /** A free slot claimed by `owner`, or 0 when the range is full. */
    seL4_CPtr alloc(uint32_t owner) noexcept
    {
        if (owners_ == nullptr) {
            return 0;
        }
        for (seL4_Word i = 0; i < count_; ++i) {
            if (owners_[i] == 0) {
                owners_[i] = owner;
                return first_ + i;
            }
        }
        return 0;
    }

    /** Return one slot `owner` reserved. */
    void free(seL4_CPtr slot, uint32_t owner) noexcept
    {
        if (owners_ != nullptr && slot >= first_ && slot < first_ + count_ &&
            owners_[slot - first_] == owner) {
            owners_[slot - first_] = 0;
        }
    }

    /** Return every slot `owner` holds. Valid only after the revoke that
     *  emptied them: a live capability left in a freed slot is handed out
     *  again, and the kernel reports the second occupant. */
    void free_owner(uint32_t owner) noexcept
    {
        if (owners_ == nullptr) {
            return;
        }
        for (seL4_Word i = 0; i < count_; ++i) {
            if (owners_[i] == owner) {
                owners_[i] = 0;
            }
        }
    }

    seL4_Word used() const noexcept
    {
        seL4_Word used = 0;
        for (seL4_Word i = 0; owners_ != nullptr && i < count_; ++i) {
            if (owners_[i] != 0) {
                ++used;
            }
        }
        return used;
    }

private:
    seL4_CPtr first_ = 0;
    seL4_Word count_ = 0;
    uint32_t *owners_ = nullptr;
};

}  // namespace aegir::mem

#endif  // AEGIR_MEM_SLOT_POOL_H
