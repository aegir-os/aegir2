/*
 * Our own address space -- implementation. See include/aegir/mem/vspace.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/mem/vspace.h>

#include <aegir/mem/allocator.h>

/* The end of our image, from the linker script the build already uses. It is
 * the image's own end rather than a number chosen here, which is the point:
 * things that move when the code grows must not be assumptions. */
extern "C" {
extern char _end[];
}

namespace aegir::mem {

namespace {
constexpr uintptr_t kPage = 1ull << seL4_PageBits;
constexpr uintptr_t kLargePage = 1ull << seL4_LargePageBits;

uintptr_t align_up(uintptr_t value) noexcept
{
    return (value + kPage - 1) & ~(kPage - 1);
}
}  // namespace

Scratch::Scratch(seL4_BootInfo *bootinfo) noexcept
    : bootinfo_(bootinfo), base_(0), next_(0), limit_(0), mapped_bytes_(0)
{
}

bool Scratch::initialise() noexcept
{
    if (bootinfo_ == nullptr) {
        return false;
    }

    uintptr_t start = reinterpret_cast<uintptr_t>(_end);
    uintptr_t ipc_end = reinterpret_cast<uintptr_t>(bootinfo_->ipcBuffer) + kPage;
    uintptr_t bootinfo_end = reinterpret_cast<uintptr_t>(bootinfo_) + kPage +
                             static_cast<uintptr_t>(bootinfo_->extraBIPages.end -
                                                     bootinfo_->extraBIPages.start) * kPage;
    if (ipc_end > start) {
        start = ipc_end;
    }
    if (bootinfo_end > start) {
        start = bootinfo_end;
    }

    base_ = align_up(start);
    next_ = base_;
    /* The large-page region containing the base already has page tables: our
     * image's last page is in it. */
    limit_ = (base_ & ~(kLargePage - 1)) + kLargePage;
    return true;
}

bool Scratch::adopt(seL4_CPtr vspace_root, uintptr_t base, uintptr_t limit,
                    Allocator *tables) noexcept
{
    if (vspace_root == 0 || tables == nullptr || base >= limit ||
        (base & (kPage - 1)) != 0 || (limit & (kPage - 1)) != 0) {
        return false;
    }
    root_ = vspace_root;
    tables_ = tables;
    base_ = base;
    next_ = base;
    limit_ = limit;
    return true;
}

void *Scratch::map(seL4_CPtr frame) noexcept
{
    if (frame == 0 || next_ + kPage > limit_) {
        return nullptr;
    }
    uintptr_t address = next_;
    seL4_Error error = seL4_RISCV_Page_Map(frame, root_, address, seL4_AllRights,
                                           seL4_RISCV_Default_VMAttributes);
    /* A service's window has no page tables promised above it: the spawner said
     * where the window is, not that anything was ever mapped there. The kernel
     * says which level is missing by refusing with FailedLookup, and the table
     * to create is a page table at whatever level failed -- the same idiom the
     * child VSpace walks (aegir/mem/child_vspace.cc), against our own root. */
    unsigned attempts = 0;
    while (error == seL4_FailedLookup && tables_ != nullptr && attempts < 4) {
        ++attempts;
        seL4_Error created = seL4_NoError;
        /* Charged to a throwaway account: the window's tables are the service's
         * own scaffolding, and the caller's accounts are per-spawn. */
        Account self{"scratch", 0, 0, 0};
        seL4_CPtr const table = tables_->alloc_object(seL4_RISCV_PageTableObject,
                                                      seL4_PageTableBits, self, &created);
        if (table == 0) {
            return nullptr;
        }
        if (seL4_RISCV_PageTable_Map(table, root_, address,
                                     seL4_RISCV_Default_VMAttributes) != seL4_NoError) {
            return nullptr;
        }
        error = seL4_RISCV_Page_Map(frame, root_, address, seL4_AllRights,
                                    seL4_RISCV_Default_VMAttributes);
    }
    if (error != seL4_NoError) {
        last_error_ = error;
        return nullptr;
    }
    last_cap_ = frame;
    next_ += kPage;
    mapped_bytes_ += kPage;
    return reinterpret_cast<void *>(address);
}

void Scratch::unmap(seL4_CPtr frame) noexcept
{
    if (frame == 0) {
        return;
    }
    seL4_RISCV_Page_Unmap(frame);
    if (frame == last_cap_) {
        /* The strict map-write-unmap rhythm hands its page back (see the header):
         * the next map reuses it rather than walking the window to its end. */
        next_ -= kPage;
        last_cap_ = 0;
    }
}

}  // namespace aegir::mem
