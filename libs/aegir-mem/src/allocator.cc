/*
 * Aegir's own allocator -- implementation. See include/aegir/mem/allocator.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The CSpace addressing used here is the recipe the pinned seL4 tree uses for a
 * single-level root CSpace: the destination is named by the root CNode's own cap
 * at full address depth, because the kernel gives that CNode a guard sized to
 * the rest of the address word
 * (projects/seL4_libs/libsel4allocman/src/bootstrap.c:434-440 sets
 * cnode_guard_bits = seL4_WordBits - cnode_size_bits).
 */

#include <aegir/mem/allocator.h>

namespace aegir::mem {

namespace {
/* Depth at which the root CNode cap itself is addressed. */
constexpr seL4_Word kRootCNodeDepth = seL4_WordBits;
}  // namespace

Allocator::Allocator(seL4_BootInfo *bootinfo) noexcept
    : bootinfo_(bootinfo), untyped_count_(0), cnode_size_bits_(0), slots_first_(0),
      slots_next_(0), slots_end_(0), slots_used_(0), normal_bytes_(0), device_bytes_(0),
      allocated_bytes_(0), last_request_bits_(0), last_candidate_bits_(0)
{
    for (auto &entry : untyped_) {
        entry = Untyped{0, 0, 0, 0, 0, 0};
    }
}

bool Allocator::initialise() noexcept
{
    if (bootinfo_ == nullptr) {
        return false;
    }

    /* The untyped caps sit in one contiguous run of slots -- a slot region
     * has a start and an end, not a count -- and the kernel describes each of
     * them at the same index (kernel/libsel4/include/sel4/bootinfo_types.h:41-53,
     * :74-75). */
    seL4_Word const untyped_caps = bootinfo_->untyped.end - bootinfo_->untyped.start;
    if (untyped_caps > static_cast<seL4_Word>(CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS)) {
        return false;
    }
    for (seL4_Word i = 0; i < untyped_caps; ++i) {
        seL4_UntypedDesc const &desc = bootinfo_->untypedList[i];
        if (!remember(bootinfo_->untyped.start + i, desc.sizeBits, desc.isDevice != 0,
                      desc.paddr)) {
            return false;
        }
        if (desc.isDevice != 0) {
            device_bytes_ += 1ull << desc.sizeBits;
        } else {
            normal_bytes_ += 1ull << desc.sizeBits;
        }
    }

    /* Slots the kernel left null for us to use (bootinfo_types.h:63). */
    cnode_size_bits_ = static_cast<unsigned>(bootinfo_->initThreadCNodeSizeBits);
    slots_first_ = bootinfo_->empty.start;
    slots_next_ = bootinfo_->empty.start;
    slots_end_ = bootinfo_->empty.end;
    return true;
}

bool Allocator::remember(seL4_CPtr cap, seL4_Word size_bits, bool device, uint64_t paddr) noexcept
{
    /* Never silently: an untyped this allocator cannot remember is memory it
     * would hand out twice or lose, and both are worse than failing. */
    if (untyped_count_ >= static_cast<unsigned>(CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS) * 8) {
        return false;
    }
    untyped_[untyped_count_].cap = cap;
    untyped_[untyped_count_].physical = paddr;
    untyped_[untyped_count_].size_bits = static_cast<uint8_t>(size_bits);
    untyped_[untyped_count_].device = device ? 1 : 0;
    untyped_[untyped_count_].used = 0;
    untyped_[untyped_count_].children = 0;
    ++untyped_count_;
    return true;
}

seL4_CPtr Allocator::alloc_slot() noexcept
{
    if (slots_next_ >= slots_end_) {
        return 0;
    }
    ++slots_used_;
    return slots_next_++;
}

void Allocator::slot_failed(seL4_CPtr slot) noexcept
{
    if (slots_next_ != 0 && slot + 1 == slots_next_) {
        --slots_next_;
        --slots_used_;
    }
}

unsigned Allocator::untyped_free() const noexcept
{
    unsigned free = 0;
    for (unsigned i = 0; i < untyped_count_; ++i) {
        if (untyped_[i].used == 0) {
            ++free;
        }
    }
    return free;
}

unsigned Allocator::object_bits(seL4_Word type, seL4_Word size_bits) noexcept
{
    if (type == seL4_CapTableObject) {
        return static_cast<unsigned>(size_bits + seL4_SlotBits);
    }
    return static_cast<unsigned>(size_bits);
}

unsigned Allocator::largest_free_bits() const noexcept
{
    unsigned largest = 0;
    for (unsigned i = 0; i < untyped_count_; ++i) {
        if (untyped_[i].used == 0 && untyped_[i].device == 0 && untyped_[i].size_bits > largest) {
            largest = untyped_[i].size_bits;
        }
    }
    return largest;
}

/* `memory_bits` is always the memory an object costs (see object_bits). */
int Allocator::find_untyped(seL4_Word memory_bits) const noexcept
{
    int best = -1;
    for (unsigned i = 0; i < untyped_count_; ++i) {
        Untyped const &entry = untyped_[i];
        if (entry.used != 0 || entry.device != 0 ||
            static_cast<seL4_Word>(entry.size_bits) < memory_bits) {
            continue;
        }
        if (best < 0 || entry.size_bits < untyped_[best].size_bits) {
            /* both are uint8_t, so this compares as int */
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool Allocator::split_to(int index, seL4_Word memory_bits, seL4_CPtr *last_child) noexcept
{
    if (last_child != nullptr) {
        *last_child = 0;
    }
    while (static_cast<seL4_Word>(untyped_[index].size_bits) > memory_bits) {
        seL4_Word half = untyped_[index].size_bits - 1;
        seL4_CPtr slot = alloc_slot();
        if (slot == 0) {
            return false;
        }
        /* Retyping an untyped out of an untyped carves the child from the LOW end of
         * what is left: the kernel creates the new object at the untyped's free index
         * and advances the index past it (kernel/src/object/untyped.c:225-232 aligns
         * the free pointer, :294-302 retypes there and moves capFreeIndex). So the
         * child sits at this entry's recorded physical base, and the remainder -- the
         * cap this entry keeps -- begins one half higher. Bookkeeping that says
         * otherwise hands a caller a physical address its memory does not have: a
         * device given it reads an empty ring out of somebody else's RAM, forever. */
        seL4_Error error =
            seL4_Untyped_Retype(untyped_[index].cap, seL4_UntypedObject, half,
                                seL4_CapInitThreadCNode, seL4_CapInitThreadCNode,
                                cnode_depth_, slot, 1);
        if (error != seL4_NoError) {
            slot_failed(slot);
            return false;
        }
        if (!remember(slot, half, false, untyped_[index].physical)) {
            return false;
        }
        if (untyped_[index].physical != 0) {
            untyped_[index].physical += 1ull << half;
        }
        untyped_[index].size_bits = static_cast<uint8_t>(half);
        untyped_[index].children = 1;
        if (last_child != nullptr) {
            *last_child = slot;
        }
    }
    return true;
}

seL4_CPtr Allocator::alloc_object(seL4_Word type, seL4_Word size_bits, Account &account,
                                  seL4_Error *error) noexcept
{
    *error = seL4_NoError;
    unsigned const wanted = object_bits(type, size_bits);
    last_request_bits_ = static_cast<unsigned>(size_bits);
    last_candidate_bits_ = 0;
    int index = find_untyped(wanted);
    if (index < 0) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    last_candidate_bits_ = untyped_[index].size_bits;
    if (!split_to(index, wanted)) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }

    seL4_CPtr slot = alloc_slot();
    if (slot == 0) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    *error = seL4_Untyped_Retype(untyped_[index].cap, type, size_bits,
                                 seL4_CapInitThreadCNode, seL4_CapInitThreadCNode,
                                 cnode_depth_, slot, 1);
    if (*error != seL4_NoError) {
        slot_failed(slot);
        return 0;
    }

    /* The untyped was split to exactly the memory the object costs, so the
     * object consumed it. */
    untyped_[index].used = 1;
    account.bytes += 1ull << wanted;
    account.objects += 1;
    allocated_bytes_ += 1ull << wanted;
    return slot;
}

bool Allocator::device_window(uint64_t base_paddr, unsigned pages, seL4_CPtr *first_out,
                              seL4_Error *error) noexcept
{
    *first_out = 0;
    *error = seL4_NoError;
    if (bootinfo_ == nullptr || pages == 0) {
        *error = seL4_InvalidArgument;
        return false;
    }
    /* The untyped the kernel described as device memory *and* that covers the address
     * asked for: device memory arrives as untypeds like any other, marked as device
     * (seL4_UntypedDesc::isDevice), and a device frame can come from nowhere else. */
    seL4_Word const count = bootinfo_->untyped.end - bootinfo_->untyped.start;
    for (seL4_Word i = 0; i < count; ++i) {
        seL4_UntypedDesc const &desc = bootinfo_->untypedList[i];
        if (desc.isDevice == 0) {
            continue;
        }
        uint64_t const base = desc.paddr;
        uint64_t const need = static_cast<uint64_t>(pages) << seL4_PageBits;
        if (base_paddr < base || base_paddr - base + need > (1ull << desc.sizeBits)) {
            continue;
        }
        for (unsigned page = 0; page < pages; ++page) {
            seL4_CPtr const slot = alloc_slot();
            if (slot == 0) {
                *error = seL4_NotEnoughMemory;
                return false;
            }
            seL4_Error const retyped =
                seL4_Untyped_Retype(bootinfo_->untyped.start + i, seL4_RISCV_4K_Page,
                                    seL4_PageBits, seL4_CapInitThreadCNode,
                                    seL4_CapInitThreadCNode, cnode_depth_, slot, 1);
            if (retyped != seL4_NoError) {
                slot_failed(slot);
                *error = retyped;
                return false;
            }
            if (page == 0) {
                *first_out = slot;
            }
        }
        return true;
    }
    *error = seL4_InvalidArgument;
    return false;
}

bool Allocator::adopt_untyped(seL4_CPtr cap, seL4_Word size_bits, uint64_t paddr) noexcept
{
    return remember(cap, size_bits, false, paddr);
}

void Allocator::adopt_slots(seL4_CPtr first, seL4_Word count, seL4_Word depth) noexcept
{
    slots_first_ = first;
    slots_next_ = first;
    slots_end_ = first + count;
    slots_used_ = 0;
    cnode_depth_ = depth;
}

seL4_CPtr Allocator::carve_untyped(seL4_Word size_bits, Account &account, seL4_Error *error,
                                    uint64_t *physical_out) noexcept
{
    *error = seL4_NoError;
    int index = find_untyped(size_bits);
    if (index < 0) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    /* An exact-size untyped that splitting left over has children, and a capability
     * with children cannot be given away (see below). Splitting a larger one makes a
     * leaf that can, so that is the better candidate for a handout when there is one. */
    if (untyped_[index].children != 0 &&
        static_cast<seL4_Word>(untyped_[index].size_bits) == size_bits) {
        for (unsigned i = 0; i < untyped_count_; ++i) {
            Untyped const &entry = untyped_[i];
            if (entry.used != 0 || entry.device != 0 ||
                static_cast<seL4_Word>(entry.size_bits) <= size_bits) {
                continue;
            }
            if (untyped_[index].children != 0 ||
                entry.size_bits < untyped_[index].size_bits) {
                index = static_cast<int>(i);
            }
        }
    }
    seL4_CPtr leaf = 0;
    if (!split_to(index, size_bits, &leaf)) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    /* What is handed out must be a capability the kernel will let the caller copy:
     * giving it to a service is the point (specs/authority.md), and a capability
     * with derived objects cannot be copied ("RevokeFirst: The untyped has been
     * used to retype an object",
     * out/aegir/libsel4/include/interfaces/sel4_client.h:419). The split's last
     * leaf has nothing derived from it; the remainder it came from does. So the
     * leaf is the handout when there was one, and the remainder stays here, free.
     * The leaf is the entry remember() just appended. */
    int taken = index;
    if (leaf != 0) {
        taken = static_cast<int>(untyped_count_) - 1;
    }
    seL4_CPtr const cap = untyped_[taken].cap;
    if (physical_out != nullptr) {
        *physical_out = untyped_[taken].physical;
    }
    untyped_[taken].used = 1;
    account.bytes += 1ull << size_bits;
    account.objects += 1;
    allocated_bytes_ += 1ull << size_bits;
    return cap;
}

seL4_CPtr Allocator::carve_page(seL4_CPtr untyped_cap, Account &account,
                                seL4_Error *error) noexcept
{
    *error = seL4_NoError;
    seL4_CPtr const slot = alloc_slot();
    if (slot == 0) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    seL4_Error const retyped =
        seL4_Untyped_Retype(untyped_cap, seL4_RISCV_4K_Page, seL4_PageBits, seL4_CapInitThreadCNode,
                            seL4_CapInitThreadCNode, cnode_depth_, slot, 1);
    if (retyped != seL4_NoError) {
        slot_failed(slot);
        *error = retyped;
        return 0;
    }
    account.objects += 1;
    return slot;
}

seL4_CPtr Allocator::make_asid_pool(Account &account, seL4_Error *error) noexcept
{
    *error = seL4_NoError;
    /* A page is comfortably more than a pool needs, and the kernel refuses anything
     * smaller -- so if this is ever wrong, it is wrong out loud. */
    seL4_CPtr const untyped = carve_untyped(seL4_PageBits, account, error);
    if (untyped == 0) {
        return 0;
    }
    seL4_CPtr const pool = alloc_slot();
    if (pool == 0) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    *error = seL4_RISCV_ASIDControl_MakePool(seL4_CapASIDControl, untyped,
                                             seL4_CapInitThreadCNode, pool, kRootCNodeDepth);
    if (*error != seL4_NoError) {
        return 0;
    }
    account.objects += 1;
    return pool;
}

}  // namespace aegir::mem
