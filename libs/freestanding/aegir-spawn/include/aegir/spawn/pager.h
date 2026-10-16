/*
 * Growing a stack on a fault (specs/pager.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The stateless half of the pager: classify a fault against a watched stack
 * range, and map pages down to it in FreeBSD's step. Both spawners page with
 * it -- the launcher for a session's commands (a child-badged mem.main chunk),
 * director for the boot services after mem.main is up (the same, from the
 * endpoint it already holds) -- and neither has to repeat the page-table
 * provisioning or the `DeleteFirst` handling.
 */

#ifndef AEGIR_SPAWN_PAGER_H
#define AEGIR_SPAWN_PAGER_H

#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::spawn {

/** What a fault against a watched stack turned out to be: the range it was in,
 *  and whether the address was the reserved part (grow) or below it (overflow).
 *  `sp` is the caller's to fill for the overflow report. */
struct StackFault {
    bool overflow = false;
    uintptr_t address = 0;
    uintptr_t base = 0;
    uintptr_t top = 0;
    uint64_t max = 0;
    uint64_t sp = 0;
};

/** Where a page or a page table comes from. `carve` retypes one of `size_bits`
 *  into a slot of the caller's CSpace, or returns 0 with `*error` set; `drop`
 *  gives a slot back when a retyped table turns out to be redundant
 *  (`seL4_DeleteFirst`). `context` is the caller's -- the launcher's watched
 *  stack and its child-badged chunk, director's service record. */
struct PagerSource {
    seL4_CPtr (*carve)(void *context, seL4_Word type, seL4_Word size_bits,
                       seL4_Error *error) noexcept;
    void (*drop)(void *context, seL4_CPtr slot) noexcept;
    void *context;
};

/** Classify a fault at `address` against the watched stack `[base, top)` and,
 *  in the reserved part `[base, lo)`, map pages down to it (specs/pager.md).
 *  False when `address` is not in `[base, top)`. True when it is:
 *  `fault->overflow` means the reservation is spent, and `*why` names a growth
 *  that ran out of something (empty on success). `*lo` is lowered as pages are
 *  mapped. */
bool page_stack(seL4_CPtr root, uintptr_t base, uintptr_t *lo, uintptr_t top,
                uint64_t max, uintptr_t address, PagerSource const &source,
                StackFault *fault, char const **why) noexcept;

}  // namespace aegir::spawn

#endif  // AEGIR_SPAWN_PAGER_H
