/*
 * Aegir's own allocator: capability slots and untyped memory.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The root task is handed the machine -- every untyped capability the kernel
 * enumerated, plus the empty slots in its own CSpace (specs/director.md). This
 * turns that grant into objects. It is deliberately ours rather than
 * libsel4utils': see specs/userland.md for why, and specs/director.md for the
 * spawn path it feeds.
 *
 * Two properties are enforced here rather than assumed:
 *
 *   - Nothing is invented. The untyped list, the free slot range and the sizes
 *     all come from the bootinfo or from the kernel's own constants; there is no
 *     capacity table of ours to keep in sync (project rule: capacity tables grow
 *     on demand). When the slot range runs out, allocation fails loudly and
 *     growing the CSpace is the next piece of work -- not a number quietly
 *     chosen here.
 *   - Everything is charged. Every object is retyped out of an untyped an
 *     account holds, so accounting is a side effect of allocating rather than a
 *     second bookkeeping pass (specs/authority.md).
 */

#ifndef AEGIR_MEM_ALLOCATOR_H
#define AEGIR_MEM_ALLOCATOR_H

#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::mem {

/** What an allocation is charged to (specs/authority.md). */
struct Account {
    char const *name;
    uint64_t bytes;
    uint64_t objects;
    uint64_t slots;
};

class Allocator {
public:
    explicit Allocator(seL4_BootInfo *bootinfo) noexcept;

    /** Index the untyped memory and the free slots the kernel reported. */
    bool initialise() noexcept;

    /** A free slot in our CSpace root, or 0 when the range is exhausted. */
    seL4_CPtr alloc_slot() noexcept;

    /**
     * Retype one object of `type`/`size_bits` out of untyped memory, splitting a
     * larger untyped when that is what is available, and charge it to `account`.
     * Returns 0 and writes `*error` on failure.
     *
     * `size_bits` is what `seL4_Untyped_Retype` wants for that type, which is not
     * always the memory the object costs: the allocator works the memory out with
     * `object_bits` so that a caller does not have to know the kernel's rule.
     */
    seL4_CPtr alloc_object(seL4_Word type, seL4_Word size_bits, Account &account,
                           seL4_Error *error) noexcept;

    /**
     * The memory an object of `type`/`size_bits` needs, in bits -- the kernel's
     * own rule (kernel/src/object/objecttype.c:42-48). A CNode is the case that
     * bites: its retype size is the number of slot bits, but it costs those plus
     * seL4_SlotBits of capability bookkeeping, so an allocator that splits to the
     * number it was handed asks the kernel for five more bits of memory than it
     * reserved and is refused.
     */
    static unsigned object_bits(seL4_Word type, seL4_Word size_bits) noexcept;

    /* The last failed object allocation, for the boot report: a spawn that fails
     * should say what it wanted and what it found. */
    unsigned last_request_bits() const noexcept { return last_request_bits_; }
    unsigned last_candidate_bits() const noexcept { return last_candidate_bits_; }
    /** The largest untyped that is still ours to allocate from. */
    unsigned largest_free_bits() const noexcept;

    /* What the machine gave us, for the boot report. */
    unsigned untyped_count() const noexcept { return untyped_count_; }
    unsigned untyped_free() const noexcept;
    uint64_t normal_bytes() const noexcept { return normal_bytes_; }
    uint64_t device_bytes() const noexcept { return device_bytes_; }
    uint64_t allocated_bytes() const noexcept { return allocated_bytes_; }
    unsigned cnode_size_bits() const noexcept { return cnode_size_bits_; }
    unsigned slots_total() const noexcept { return slots_end_ - slots_first_; }
    unsigned slots_used() const noexcept { return slots_used_; }

private:
    /* One untyped capability: 2^size_bits bytes, device or normal. Storing the
     * size is what lets the allocator split and fit without asking the kernel. */
    struct Untyped {
        seL4_CPtr cap;
        uint8_t size_bits;
        uint8_t device;
        uint8_t used;
    };

    /** Index of the smallest unused normal untyped that can hold `size_bits`. */
    int find_untyped(seL4_Word size_bits) const noexcept;

    /** Halve `untyped_[index]` until it is exactly `size_bits` wide. */
    bool split_to(int index, seL4_Word size_bits) noexcept;

    bool remember(seL4_CPtr cap, seL4_Word size_bits, bool device) noexcept;

    seL4_BootInfo *bootinfo_;
    /* Room for the kernel's own list *and* the halves splitting creates: every
     * allocation from a big untyped can add up to one entry per halving. It is a
     * bootstrap structure, so it is static storage rather than something the
     * allocator allocates -- and when it is full, allocations fail loudly rather
     * than quietly forgetting memory (remember() returns false). */
    Untyped untyped_[CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS * 8];
    unsigned untyped_count_;
    unsigned cnode_size_bits_;
    seL4_CPtr slots_first_;
    seL4_CPtr slots_next_;
    seL4_CPtr slots_end_;
    unsigned slots_used_;
    uint64_t normal_bytes_;
    uint64_t device_bytes_;
    uint64_t allocated_bytes_;
    unsigned last_request_bits_;
    unsigned last_candidate_bits_;
};

}  // namespace aegir::mem

#endif  // AEGIR_MEM_ALLOCATOR_H
