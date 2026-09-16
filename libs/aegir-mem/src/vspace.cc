/*
 * Our own address space -- implementation. See include/aegir/mem/vspace.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/mem/vspace.h>

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

void *Scratch::map(seL4_CPtr frame) noexcept
{
    if (frame == 0 || next_ + kPage > limit_) {
        return nullptr;
    }
    uintptr_t address = next_;
    seL4_Error error = seL4_RISCV_Page_Map(frame, seL4_CapInitThreadVSpace, address,
                                          seL4_AllRights, seL4_RISCV_Default_VMAttributes);
    if (error != seL4_NoError) {
        return nullptr;
    }
    next_ += kPage;
    mapped_bytes_ += kPage;
    return reinterpret_cast<void *>(address);
}

void Scratch::unmap(seL4_CPtr frame) noexcept
{
    if (frame != 0) {
        seL4_RISCV_Page_Unmap(frame);
    }
}

}  // namespace aegir::mem
