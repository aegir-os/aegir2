/*
 * Growing a region on a fault -- implementation. See
 * include/aegir/spawn/pager.h and specs/pager.md.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/spawn/pager.h>

#include <aegir/bootstrap.h>

namespace aegir::spawn {

namespace {

constexpr uint64_t kPage = 1ull << seL4_PageBits;

/* FreeBSD's SGROWSIZ: how far down a stack grows on the fault that needs
 * another page. A fault deeper than this maps everything between it and the
 * mapped base, so one jump is one call however far it is. */
constexpr uint64_t kGrowBytes = 128ull * 1024;

/* Map `frame` at `address`, creating the page tables above it if they are
 * missing. Returns the kernel's own answer: `seL4_NoError`, `seL4_DeleteFirst`
 * when something already sits at the address, or an error the caller names. */
seL4_Error map_with_tables(seL4_CPtr root, uintptr_t address, seL4_CPtr frame,
                           PagerSource const &source, char const **why) noexcept
{
    seL4_Error mapped = seL4_RISCV_Page_Map(frame, root, address, seL4_AllRights,
                                            seL4_RISCV_Default_VMAttributes);
    unsigned attempts = 0;
    while (mapped == seL4_FailedLookup && attempts < 8) {
        ++attempts;
        seL4_Error error = seL4_NoError;
        seL4_CPtr const table = source.carve(source.context, seL4_RISCV_PageTableObject,
                                             seL4_PageTableBits, &error);
        if (table == 0) {
            if (why != nullptr) {
                *why = "no page table for the region";
            }
            return seL4_NotEnoughMemory;
        }
        seL4_Error const table_mapped =
            seL4_RISCV_PageTable_Map(table, root, address, seL4_RISCV_Default_VMAttributes);
        if (table_mapped == seL4_DeleteFirst) {
            /* A table already reaches this address; ours is redundant, so its
             * capability goes. The source decides whether the slot is reusable
             * (a fallback) or spent (a run). */
            seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, table,
                              aegir::bootstrap::endpoint_depth());
            if (source.drop != nullptr) {
                source.drop(source.context, table);
            }
        } else if (table_mapped != seL4_NoError) {
            if (why != nullptr) {
                *why = "a page table could not be mapped";
            }
            return table_mapped;
        }
        mapped = seL4_RISCV_Page_Map(frame, root, address, seL4_AllRights,
                                     seL4_RISCV_Default_VMAttributes);
    }
    return mapped;
}

}  // namespace

bool page_region(seL4_CPtr root, Region &region, uintptr_t address,
                 PagerSource const &source, StackFault *fault, char const **why) noexcept
{
    if (why != nullptr) {
        *why = "";
    }
    if (address < region.guard || address >= region.top) {
        return false;
    }
    if (fault != nullptr) {
        fault->address = address;
        fault->base = region.base;
        fault->top = region.top;
        fault->max = region.base < region.top ? region.top - region.base : 0;
    }

    if (region.file_backed) {
        /* A segment page, mapped from the image the moment it is touched. */
        uintptr_t const page = address & ~(static_cast<uintptr_t>(kPage) - 1u);
        seL4_Error error = seL4_NoError;
        seL4_CPtr const frame =
            source.carve(source.context, seL4_RISCV_4K_Page, seL4_PageBits, &error);
        if (frame == 0) {
            if (why != nullptr) {
                *why = "no frame for the segment";
            }
            return true;
        }
        if (source.fill != nullptr &&
            !source.fill(source.context, region, page, frame)) {
            if (source.drop != nullptr) {
                source.drop(source.context, frame);
            }
            if (why != nullptr) {
                *why = "a segment page could not be read";
            }
            return true;
        }
        seL4_Error const mapped = map_with_tables(root, page, frame, source, why);
        if (mapped == seL4_DeleteFirst) {
            /* Another thread reached the page first; its frame is the one that
             * counts, and this one goes. Resume the faulting thread. */
            seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, frame,
                              aegir::bootstrap::endpoint_depth());
            if (source.drop != nullptr) {
                source.drop(source.context, frame);
            }
            return true;
        }
        if (mapped != seL4_NoError) {
            if (why != nullptr && (*why)[0] == '\0') {
                *why = "a segment page could not be mapped";
            }
            return true;
        }
        return true;
    }

    /* The stack. A fault below the reserved base is the guard page: the end. */
    if (address < region.base) {
        if (fault != nullptr) {
            fault->overflow = true;
        }
        return true;
    }
    if (address >= region.lo) {
        /* At or above the lowest mapped page: already mapped, so this is a
         * protection fault, not growth. */
        return false;
    }
    /* Grow by at least the step, or down to the fault when it is deeper, so a
     * stack that faults one page below its mapped base maps a run rather than a
     * page (FreeBSD's SGROWSIZ, specs/pager.md). */
    uintptr_t target = address & ~(static_cast<uintptr_t>(kPage) - 1u);
    if (region.lo > region.base + kGrowBytes) {
        uintptr_t const step = region.lo - kGrowBytes;
        if (target > step) {
            target = step;
        }
    } else {
        target = region.base;
    }
    while (region.lo > target) {
        seL4_Error error = seL4_NoError;
        seL4_CPtr const frame =
            source.carve(source.context, seL4_RISCV_4K_Page, seL4_PageBits, &error);
        if (frame == 0) {
            if (why != nullptr) {
                *why = "no frame for the stack";
            }
            return true;
        }
        uintptr_t const at = region.lo - kPage;
        seL4_Error const mapped = map_with_tables(root, at, frame, source, why);
        if (mapped != seL4_NoError) {
            if (why != nullptr && (*why)[0] == '\0') {
                *why = "a stack page could not be mapped";
            }
            return true;
        }
        region.lo = at;
    }
    return true;
}

}  // namespace aegir::spawn
