/*
 * aegir-cc: the on-device compiler's first measurement (specs/clang-on-aegir.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A spawned boot service that stands the hosted runtime up and then reads a
 * file the way clang reads a source -- through llvm::MemoryBuffer -- so the
 * first thing the on-device compiler does is measured against what Aegir
 * actually answers, rather than guessed from a gap map. This file is the
 * seL4-facing half and must not include a libc++ or LLVM header (probe.h
 * explains why); probe.cc is the LLVM-facing half.
 *
 * The next step (specs/clang-on-aegir.md Phase 3) grows this into the
 * in-process clang+lld driver; for now it proves the LLVM libraries link into
 * an Aegir program and that their file layer reaches Aegir's POSIX surface.
 */

#include "probe.h"

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <sel4/sel4.h>

namespace {

/* Static, like the smokes': the allocator's tables and the scratch window's
 * bookkeeping are tens of kilobytes, and a service's stack is pages. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

/* The mapping authority the spawn kit installs: the delegated untyped, the
 * VSpace root and the free-address window. The pattern is the cxx-smoke's. */
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
        g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::kCNodeBits) - first_free, 0,
                              aegir::bootstrap::kCNodeBits);
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

    aegir::debug_write("\naegir-cc: the on-device compiler's first measurement\n");

    if (!adopt_memory()) {
        aegir::debug_write("  cc: FAIL no untyped, vspace or window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Room for LLVM's Support globals and a file mapping; the seed is a floor
     * and the heap grows past it through mem.main (specs/memory.md Phase 2). */
    constexpr uint64_t kHeapBytes = 16ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("  cc: FAIL the heap could not claim the window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    int const failed = aegir::clang_probe::read_file("Initrd:services.manifest");

    aegir::debug_write(failed == 0 ? "AEGIR_CC_READ_OK\n" : "AEGIR_CC_READ_FAIL\n");

    /* The boot thread waits for this, so the marker can follow. */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    return failed;
}
