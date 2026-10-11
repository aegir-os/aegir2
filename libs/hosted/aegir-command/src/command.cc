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

/* The allocator and the window the heap is claimed on: function-local statics
 * on purpose. An allocator's untyped table is tens of kilobytes and a process's
 * stack is pages (specs/userland.md), so they cannot live on the stack -- but a
 * *file-scope* object is built by a dynamic initializer, and those run after
 * aegir-crt0's constructor (whose priority is low because the heap must be up
 * before a program's own constructors can allocate). That left this stand-up
 * asking a zeroed allocator -- no node pool, so the very kit it was handed was
 * refused -- which is the one thing the ordering must not do. The first ask
 * builds them instead, whoever makes it. */
aegir::mem::Allocator &objects() noexcept
{
    static aegir::mem::Allocator allocator(nullptr);
    return allocator;
}

aegir::mem::Scratch &window() noexcept
{
    static aegir::mem::Scratch scratch(nullptr);
    return scratch;
}

/* Whether this process is already up: the heap is claimed once, and a second
 * ask -- a program that calls start() on top of aegir-crt0's constructor, or
 * the other way round -- is answered rather than refused. */
bool g_stood_up = false;

/* Another L2 CNode for a CSpace that must grow (specs/memory.md, "Growing the
 * CSpace on demand"): the block gave this process a level one (`cnode_l1_bits`),
 * the CNodes in it are laid down consecutively, and `first` is the first encoded
 * slot the adopted run does not cover -- so `first`'s level-one index is the
 * slot the new CNode goes to, and a retype lands the capability straight there
 * (`Allocator::alloc_cnode_at_l1`). Growing a slot spends no slot: the object's
 * own capability *is* the level-one slot.
 *
 * The account is the heap's throwaway shape -- charged once, never reclaimed --
 * because an L2 CNode is not something a process gives back (specs/memory.md).
 * A command is spawned with a run of them sized by its image; this is what lets
 * a program whose *runtime* objects are more than its image -- a linker -- keep
 * going instead of dying of `std::bad_alloc` a layer up. */
bool grow_slots(void *context, seL4_CPtr first) noexcept
{
    static_cast<void>(context);
    aegir::mem::Account account{"cspace", 0, 0, 0};
    seL4_Error error = seL4_NoError;
    seL4_Word const l1_slot =
        static_cast<seL4_Word>(first) >> aegir::bootstrap::cnode_bits();
    return objects().alloc_cnode_at_l1(aegir::bootstrap::cnode_bits(), l1_slot, account,
                                       &error) != 0;
}

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
              objects().adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                      untyped_physical);
    if (ok) {
        uint32_t const l2 = aegir::bootstrap::cnode_bits();
        uint32_t const l1 = aegir::bootstrap::cnode_l1_bits();
        if (l1 != 0) {
            /* A two-level process: its plain slots live in the L2 CNode at its
             * root slot 0, and the rest of its level one is empty, so the CSpace
             * grows into it rather than ending at one CNode (specs/memory.md,
             * "Growing the CSpace on demand"). The spawner's half is giving a
             * command `cspace_l1_bits`; `grow_slots` above is this half. */
            objects().adopt_slots_level_two(first_free, (1u << l2) - first_free, l1, l2, 0);
            objects().set_slot_source(grow_slots, nullptr);
        } else {
            objects().adopt_slots(first_free, (1u << l2) - first_free, 0, l2);
        }
        ok = window().adopt(static_cast<seL4_CPtr>(vspace_slot),
                            static_cast<uintptr_t>(window_base),
                            static_cast<uintptr_t>(window_base + window_bytes), &objects());
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
    if (!aegir::heap::init(objects(), window(), kHeapBytes)) {
        aegir::debug_write("  ");
        aegir::debug_write(name != nullptr ? name : "command");
        aegir::debug_write(": FAIL the heap would not claim the window\n");
        return false;
    }
    g_stood_up = true;
    return true;
}

}  // namespace aegir::command
