/*
 * Growing a stack on a fault -- implementation. See
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

}  // namespace

bool page_stack(seL4_CPtr root, uintptr_t base, uintptr_t *lo, uintptr_t top,
                uint64_t max, uintptr_t address, PagerSource const &source,
                StackFault *fault, char const **why) noexcept
{
    if (why != nullptr) {
        *why = "";
    }
    if (fault != nullptr) {
        fault->address = address;
        fault->base = base;
        fault->top = top;
        fault->max = max;
    }
    if (address >= *lo) {
        /* At or above the lowest mapped page: not a growth fault. A fault in
         * the mapped part is a protection problem, and one above the top is
         * another mapping altogether. */
        return false;
    }
    if (address < base) {
        /* The guard page under the reservation, or below it: the stack is
         * spent. The caller reports and ends the thread. */
        if (fault != nullptr) {
            fault->overflow = true;
        }
        return true;
    }
    /* In [base, lo): grow by at least the step, or down to the fault when it
     * is deeper, so a stack that faults one page below its mapped base maps a
     * run rather than a page (FreeBSD's SGROWSIZ, specs/pager.md). */
    uintptr_t target = address & ~(static_cast<uintptr_t>(kPage) - 1u);
    if (*lo > base + kGrowBytes) {
        uintptr_t const step = *lo - kGrowBytes;
        if (target > step) {
            target = step;
        }
    } else {
        target = base;
    }
    while (*lo > target) {
        seL4_Error error = seL4_NoError;
        seL4_CPtr const frame =
            source.carve(source.context, seL4_RISCV_4K_Page, seL4_PageBits, &error);
        if (frame == 0) {
            if (why != nullptr) {
                *why = "no frame for the stack";
            }
            return true;
        }
        uintptr_t const at = *lo - kPage;
        seL4_Error mapped = seL4_RISCV_Page_Map(frame, root, at, seL4_AllRights,
                                                seL4_RISCV_Default_VMAttributes);
        unsigned attempts = 0;
        while (mapped == seL4_FailedLookup && attempts < 8) {
            ++attempts;
            seL4_CPtr const table = source.carve(
                source.context, seL4_RISCV_PageTableObject, seL4_PageTableBits, &error);
            if (table == 0) {
                if (why != nullptr) {
                    *why = "no page table for the stack";
                }
                return true;
            }
            seL4_Error const table_mapped = seL4_RISCV_PageTable_Map(
                table, root, at, seL4_RISCV_Default_VMAttributes);
            if (table_mapped == seL4_DeleteFirst) {
                /* A table already reaches this address; ours is redundant, so
                 * its capability goes. The source decides whether the slot is
                 * reusable (a fallback) or spent (a run). */
                seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, table,
                                  aegir::bootstrap::endpoint_depth());
                if (source.drop != nullptr) {
                    source.drop(source.context, table);
                }
            } else if (table_mapped != seL4_NoError) {
                if (why != nullptr) {
                    *why = "a stack page table could not be mapped";
                }
                return true;
            }
            mapped = seL4_RISCV_Page_Map(frame, root, at, seL4_AllRights,
                                         seL4_RISCV_Default_VMAttributes);
        }
        if (mapped != seL4_NoError) {
            if (why != nullptr) {
                *why = "a stack page could not be mapped";
            }
            return true;
        }
        *lo = at;
    }
    return true;
}

}  // namespace aegir::spawn
