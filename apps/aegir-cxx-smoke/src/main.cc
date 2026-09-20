/*
 * aegir-cxx-smoke: the hosted C++ runtime's acceptance client.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * It stands the hosted runtime up and exercises it: aegir-heap turns the
 * memory the spawn kit gave this process into musl's mallocng, and the checks
 * in checks.cc run libc++'s containers on top of it. This file is the
 * seL4-facing half and must not include a libc++ header (checks.h explains
 * why); the checks are the libc++ half.
 *
 * What it proves, in order: the untyped, the VSpace root and the window
 * arrived in the bootstrap block; musl's memory syscalls reach our dispatcher
 * (no null __sysinfo); mallocng gets pages and recycles freed ones; libc++'s
 * operator new/delete, std::string, std::vector and std::unordered_map all
 * link and run against them.
 */

#include "checks.h"

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <sel4/sel4.h>

namespace {

/* Static, like the greeter's and the test bed's: the allocator's untyped table
 * and the scratch window's bookkeeping are tens of kilobytes, and a service's
 * stack is pages (specs/userland.md). */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

/* The mapping authority the spawn kit installs: the delegated untyped (page
 * tables and frames are retyped from it), the VSpace root, and the window of
 * free addresses (the give_vspace grant). The pattern is the greeter's and the
 * test bed's. */
bool adopt_memory()
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
        g_objects.adopt_slots(first_free, (1u << 10) - first_free, 0);
        ok = g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                             static_cast<uintptr_t>(window_base),
                             static_cast<uintptr_t>(window_base + window_bytes), &g_objects);
    }
    return ok;
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("\ncxx-smoke: the hosted C++ runtime\n");

    if (!adopt_memory()) {
        aegir::debug_write("  cxx-smoke: FAIL no untyped, vspace or window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    constexpr uint64_t kHeapBytes = 8ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("  cxx-smoke: FAIL the heap could not claim the window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    int const failed = aegir::cxx_smoke::run();

    aegir::debug_write(failed == 0 ? "CXX_SMOKE_OK\n" : "CXX_SMOKE_FAIL\n");

    /* The boot thread waits for this, so the marker can follow (the same clock
     * the greeter's form-up signal is). */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
