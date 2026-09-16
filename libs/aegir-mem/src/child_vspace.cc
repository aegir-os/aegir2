/*
 * A child's address space -- implementation. See include/aegir/mem/child_vspace.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/mem/child_vspace.h>

/* seL4_MappingFailedLookupLevel and SEL4_MAPPING_LOOKUP_NO_PT: the arch header
 * that says which level a failed mapping wanted
 * (kernel/libsel4/sel4_arch_include/riscv64/sel4/sel4_arch/mapping.h:15-21). */
#include <sel4/sel4_arch/mapping.h>

namespace aegir::mem {

namespace {
constexpr uint64_t kPage = 1ull << seL4_PageBits;
}  // namespace

ChildVSpace::ChildVSpace(Allocator &allocator, Scratch &scratch) noexcept
    : allocator_(allocator), scratch_(scratch), root_(0), mapped_pages_(0), mapped_bytes_(0)
{
}

bool ChildVSpace::create(seL4_CPtr pool, Account &account) noexcept
{
    seL4_Error error = seL4_NoError;
    /* The root of a RISC-V VSpace is a page table, and the kernel is content to
     * be handed it as the `vspace` argument of every map below. */
    root_ = allocator_.alloc_object(seL4_RISCV_PageTableObject, seL4_PageTableBits, account, &error);
    if (root_ == 0) {
        return false;
    }
    /* A VSpace root is unusable until it has an address space id, and a TCB
     * cannot be configured with it (the kernel refuses with "not assigned to an
     * ASID pool",
     * out/aegir/libsel4/include/interfaces/sel4_client.h:884). The pool is the
     * caller's -- the spawner's -- and this is the step that makes "an address
     * space is built from a pool" true in specs/authority.md. */
    return seL4_RISCV_ASIDPool_Assign(pool, root_) == seL4_NoError;
}

bool ChildVSpace::map_page(uintptr_t address, seL4_CPtr frame, bool writable,
                           Account &account, seL4_Error *error_out) noexcept
{
    if (error_out != nullptr) {
        *error_out = seL4_NoError;
    }
    if (root_ == 0 || frame == 0) {
        if (error_out != nullptr) {
            *error_out = seL4_InvalidCapability;
        }
        return false;
    }
    seL4_CapRights_t const rights = writable ? seL4_AllRights : seL4_CanRead;
    seL4_Error error = seL4_RISCV_Page_Map(frame, root_, address, rights,
                                           seL4_RISCV_Default_VMAttributes);

    /* The page tables above this page may not exist yet. Which one is missing is
     * in the error, but there is no need to ask: on RISC-V the object to create
     * is a page table at whatever level failed -- the arch helper ignores the
     * level entirely and returns seL4_RISCV_PageTableObject
     * (projects/seL4_libs/libsel4vspace/src/arch/riscv/mapping.c:18-27) -- and a
     * fresh VSpace needs both the intermediate table and the one above it, so
     * looping here creates the tree the mapping needs, one level per pass. */
    unsigned attempts = 0;
    while (error == seL4_FailedLookup && attempts < 8) {
        ++attempts;
        seL4_Error created = seL4_NoError;
        seL4_CPtr table = allocator_.alloc_object(seL4_RISCV_PageTableObject, seL4_PageTableBits,
                                                  account, &created);
        if (table == 0) {
            return false;
        }
        /* Mapped at the address being mapped, not at its region's base: the
         * kernel places the table in the slot for that address. */
        seL4_Error mapped =
            seL4_RISCV_PageTable_Map(table, root_, address, seL4_RISCV_Default_VMAttributes);
        if (mapped != seL4_NoError) {
            return false;
        }
        error = seL4_RISCV_Page_Map(frame, root_, address, rights, seL4_RISCV_Default_VMAttributes);
    }
    if (error != seL4_NoError) {
        if (error_out != nullptr) {
            *error_out = error;
        }
        return false;
    }
    ++mapped_pages_;
    mapped_bytes_ += kPage;
    return true;
}

bool ChildVSpace::populate(uintptr_t address, unsigned pages, void const *source, uint64_t bytes,
                           uint64_t leading, bool writable, Account &account,
                           seL4_CPtr *first_frame, char const **why) noexcept
{
    if (why != nullptr) {
        *why = "";
    }
    if (leading >= kPage || bytes + leading > static_cast<uint64_t>(pages) * kPage) {
        if (why != nullptr) {
            *why = "the bytes do not fit the pages";
        }
        return false;
    }

    auto const *input = static_cast<unsigned char const *>(source);
    uint64_t remaining = bytes;
    uint64_t skip = leading;
    for (unsigned page = 0; page < pages; ++page) {
        seL4_Error error = seL4_NoError;
        seL4_CPtr frame =
            allocator_.alloc_object(seL4_RISCV_4K_Page, seL4_PageBits, account, &error);
        if (frame == 0) {
            if (why != nullptr) {
                *why = "no memory for a frame";
            }
            return false;
        }
        if (page == 0 && first_frame != nullptr) {
            *first_frame = frame;
        }

        /* Fill it first, through our own window: once it is mapped into the
         * child there is no way for us to reach it. */
        void *window = scratch_.map(frame);
        if (window == nullptr) {
            if (why != nullptr) {
                *why = "the window the frame is filled through is full";
            }
            return false;
        }
        auto *destination = static_cast<unsigned char *>(window);
        for (uint64_t i = 0; i < kPage; ++i) {
            destination[i] = 0;
        }
        uint64_t const room = kPage - skip;
        uint64_t const chunk = remaining < room ? remaining : room;
        for (uint64_t i = 0; i < chunk; ++i) {
            destination[skip + i] = input[i];
        }
        skip = 0;
        scratch_.unmap(frame);

        if (!map_page(address + static_cast<uintptr_t>(page) * kPage, frame, writable, account)) {
            if (why != nullptr) {
                *why = "a page could not be mapped into the child";
            }
            return false;
        }
        input += chunk;
        remaining -= chunk;
    }
    return true;
}

}  // namespace aegir::mem
