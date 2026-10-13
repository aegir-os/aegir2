/*
 * aegir-posix-conformance: the POSIX conformance suite.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Derived from The Open Group Base Specifications Issue 8 (IEEE Std
 * 1003.1-2024), fetched by `make deps` into third_party/posix-spec/issue8 as a
 * *reference* (its text is not redistributed). Every case names the interface it
 * belongs to; `scripts/check_posix_coverage.py` cross-reads those names against
 * `manifests/posix-surface.toml` and the dispatcher, so an interface with no case
 * is a failed check and not a silence (specs/posix-conformance.md).
 *
 * Four kinds of line, and one marker:
 *   posix: ok|FAIL  <interface>: <test>   -- an implemented interface
 *   posix: hole     <interface>: <test>   -- an interface Aegir does not
 *                                            implement; the call must fail, and
 *                                            the errno is printed
 *   posix: deviate  <interface>: <test>   -- a deliberate non-conformance
 *   posix: SKIP     <interface>: <test>   -- not testable in this process
 *   AEGIR_POSIX_CONFORMANCE_OK|FAIL <passed>/<total> (<skipped> skipped,
 *                                                 <holes> holes, <deviations> deviations)
 *
 * A boot service: it stands itself up (the untyped, the VSpace root and the
 * window the spawn kit installed, then the heap) before its first printf.
 * `vfs.namespace` lets the files and directories areas open real files.
 */

#include "harness.h"
#include "surface.h"

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <sel4/sel4.h>
#include <stdio.h>

namespace {

/* Static, like the greeter's, the test bed's and the C++ smoke's: the
 * allocator's untyped table and the scratch window's bookkeeping are tens of
 * kilobytes, and a service's stack is pages (specs/userland.md). */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

/* The mapping authority the spawn kit installs: the delegated untyped (page
 * tables and frames are retyped from it), the VSpace root, and the window of
 * free addresses (the give_vspace grant). The pattern is the C++ smoke's, the
 * greeter's and the test bed's. */
bool adopt_memory()
{
    uint64_t untyped_slot = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    uint64_t untyped_physical = 0;
    uint32_t untyped_bits = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(aegir::bootstrap::untyped(&untyped_physical, &untyped_bits, &untyped_address));

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
        g_objects.adopt_slots_level_two(first_free, (1u << aegir::bootstrap::cnode_bits()) - first_free,
                                        aegir::bootstrap::cnode_l1_bits(),
                                        aegir::bootstrap::cnode_bits(), 0);
        ok = g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                             static_cast<uintptr_t>(window_base),
                             static_cast<uintptr_t>(window_base + window_bytes), &g_objects);
    }
    return ok;
}

}  // namespace

int main()
{
    if (!adopt_memory()) {
        aegir::debug_write("  posix-conformance: FAIL no untyped, vspace or window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    constexpr uint64_t kHeapBytes = 8ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("  posix-conformance: FAIL the heap could not claim the window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    aegir_posix_conformance::memory_surface();
    /* The files area against both filesystems, with the expected result a
     * property of the volume (scratch.h). */
    aegir_posix_conformance::files_surface(aegir_posix_conformance::kFatVolume);
    aegir_posix_conformance::files_surface(aegir_posix_conformance::kBfsVolume);
    aegir_posix_conformance::directories_surface(aegir_posix_conformance::kFatVolume);
    aegir_posix_conformance::directories_surface(aegir_posix_conformance::kBfsVolume);
    aegir_posix_conformance::process_surface();
    aegir_posix_conformance::environment_surface();
    aegir_posix_conformance::time_surface();
    aegir_posix_conformance::sockets_surface();
    aegir_posix_conformance::signals_surface();

    int const passed = aegir_posix_conformance::passed();
    int const failed = aegir_posix_conformance::failed();
    int const skipped = aegir_posix_conformance::skipped();
    int const holes = aegir_posix_conformance::holes();
    int const deviations = aegir_posix_conformance::deviations();
    /* Three outcomes, because "the suite ran" and "Aegir conforms" are two
     * claims: FAIL when a case failed, CONFORMS only when nothing is a hole or
     * a deviation, and GAPS in between -- the run finished, and the counts are
     * how much of the standard is not met. A pass is the second one only. */
    char const *verdict = "GAPS";
    if (failed != 0) {
        verdict = "FAIL";
    } else if (holes == 0 && deviations == 0) {
        verdict = "CONFORMS";
    }
    printf("AEGIR_POSIX_CONFORMANCE_%s %d/%d (%d skipped, %d holes, %d deviations)\n", verdict,
           passed, passed + failed, skipped, holes, deviations);
    (void)fflush(nullptr);

    /* The boot thread waits for this signal (the same one the greeter and the
     * C++ smoke send), so the marker can follow it. Returning, rather than
     * _exit, lets the runtime's exit bridge run. */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    return failed;
}
