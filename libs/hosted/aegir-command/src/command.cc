/*
 * aegir::command -- implementation. See command.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "aegir/command.h"

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <sel4/sel4.h>

namespace aegir::command {

namespace {

/* Static, like every hosted program's: the allocator's untyped table is tens
 * of kilobytes and a process's stack is pages (specs/userland.md). */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

/* Whether this process is already up: the heap is claimed once, and a second
 * ask -- a program that calls start() on top of aegir-crt0's constructor, or
 * the other way round -- is answered rather than refused. */
bool g_stood_up = false;

bool adopt_memory() noexcept
{
    uint64_t untyped_slot = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    uint64_t untyped_physical = 0;
    uint32_t untyped_bits = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(aegir::bootstrap::untyped(&untyped_physical, &untyped_bits,
                                                &untyped_address));

    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind == aegir::bootstrap::EntryKind::Capability &&
                entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        }
    }

    bool ok = aegir::bootstrap::capability("untyped", 7, &untyped_slot) &&
              aegir::bootstrap::capability("vspace", 6, &vspace_slot) &&
              aegir::bootstrap::window(&window_base, &window_bytes) &&
              g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                      untyped_physical);
    if (ok) {
        g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::cnode_bits()) - first_free, 0,
                          aegir::bootstrap::cnode_bits());
        ok = g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                             static_cast<uintptr_t>(window_base),
                             static_cast<uintptr_t>(window_base + window_bytes), &g_objects);
    }
    return ok;
}

}  // namespace

bool start(char const *name) noexcept
{
    /* The runtime may have stood this process up already: aegir-crt0's
     * constructor runs before main, so a program that carries no Aegir call is
     * up when it starts and the call this one carries is a no-op. One heap per
     * process is the rule (heap::init's own ready_ guard), so a second ask is
     * answered here rather than refused. */
    if (g_stood_up) {
        return true;
    }
    if (!adopt_memory()) {
        aegir::debug_write("  ");
        aegir::debug_write(name != nullptr ? name : "command");
        aegir::debug_write(": FAIL no untyped, vspace or window\n");
        return false;
    }
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("  ");
        aegir::debug_write(name != nullptr ? name : "command");
        aegir::debug_write(": FAIL the heap would not claim the window\n");
        return false;
    }
    g_stood_up = true;
    return true;
}

}  // namespace aegir::command
