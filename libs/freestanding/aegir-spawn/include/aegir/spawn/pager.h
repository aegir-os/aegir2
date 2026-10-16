/*
 * Growing a region on a fault (specs/pager.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The stateless half of the pager: classify a fault against a watched region
 * and map the page it asks for. Both spawners page with it -- the launcher for
 * a session's commands, director for the boot services after mem.main is up --
 * and neither repeats the page-table provisioning or the `DeleteFirst`
 * handling.
 *
 * A region is the one variable (specs/pager.md's "The page source is the one
 * variable"): the **stack** is anonymous -- it grows down, and `lo` is its
 * lowest mapped page; an **ELF segment** (file-backed) maps any of its pages on
 * demand from the image the spawner retained. The stack uses `carve`/`drop`
 * (a frame from memory); a segment also uses `fill` (the page read from the
 * file, or zeroed past the segment's file bytes).
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

/** A watched range the pager serves.
 *
 *  - The **stack** (`file_backed == false`) grows down: `lo` is its lowest
 *    mapped page, a fault in `[base, lo)` maps a page, and a fault in
 *    `[guard, base)` is the overflow.
 *  - A **segment** (`file_backed == true`) maps any page of `[base, top)` on
 *    demand: the page's bytes come from `file_offset + (address - vaddr)` for
 *    the first `file_bytes`, and the rest are zero. */
struct Region {
    uintptr_t guard = 0;       /* the watched bottom: the stack's guard page, or the
                                  segment's own base */
    uintptr_t base = 0;        /* the reserved bottom */
    uintptr_t top = 0;         /* the reserved top, exclusive */
    uintptr_t lo = 0;          /* the lowest mapped page (the stack's cursor); == top
                                  for a segment */
    bool file_backed = false;
    uintptr_t vaddr = 0;       /* the segment's first byte */
    uint64_t file_offset = 0;  /* the image offset of `vaddr` */
    uint64_t file_bytes = 0;   /* bytes at `vaddr` that are file-backed */
};

/** Where a page or a page table comes from. `carve` retypes one of `size_bits`
 *  into a slot of the caller's CSpace, or returns 0 with `*error` set; `drop`
 *  gives a slot back when a retyped table turns out to be redundant
 *  (`seL4_DeleteFirst`); `fill` writes one page for a fault at `address` in
 *  `region`, through the caller's own window or image -- the anonymous arm
 *  zeroes it, the file-backed arm reads the segment's bytes and zeroes the
 *  rest. `context` is the caller's. */
struct PagerSource {
    seL4_CPtr (*carve)(void *context, seL4_Word type, seL4_Word size_bits,
                       seL4_Error *error) noexcept;
    void (*drop)(void *context, seL4_CPtr slot) noexcept;
    bool (*fill)(void *context, Region const &region, uintptr_t address,
                 seL4_CPtr frame) noexcept;
    void *context;
};

/** Serve a fault at `address` against `region`. False when `address` is not in
 *  the watched range. True when it is: `fault->overflow` means the stack ran
 *  past its base, and `*why` names a growth that ran out of something (empty on
 *  success). `region.lo` is lowered as stack pages are mapped. */
bool page_region(seL4_CPtr root, Region &region, uintptr_t address,
                 PagerSource const &source, StackFault *fault, char const **why) noexcept;

}  // namespace aegir::spawn

#endif  // AEGIR_SPAWN_PAGER_H
