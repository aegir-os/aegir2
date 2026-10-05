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

#include <new>

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <sel4/sel4.h>

namespace {

/* Storage, not objects: the allocator and the scratch window are constructed in
 * cc_claim_heap (constructor 300), ahead of the library's own constructors, so a
 * library global constructor that allocates has a heap. As plain globals they
 * would be constructed in the default-priority phase -- after that point, and
 * interleaved with the library's -- so the ordering could not be relied on.
 * Static, like the smokes': their tables are tens of kilobytes and a process's
 * stack is pages. */
alignas(aegir::mem::Allocator) unsigned char g_objects_storage[sizeof(aegir::mem::Allocator)];
alignas(aegir::mem::Scratch) unsigned char g_scratch_storage[sizeof(aegir::mem::Scratch)];
aegir::mem::Allocator *g_objects = nullptr;
aegir::mem::Scratch *g_scratch = nullptr;

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
              g_objects->adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                       untyped_physical);
    if (ok) {
        uint32_t const bits = aegir::bootstrap::cnode_bits();
        g_objects->adopt_slots(first_free, (1u << bits) - first_free, 0, bits);
        ok = g_scratch->adopt(static_cast<seL4_CPtr>(vspace_slot),
                              static_cast<uintptr_t>(window_base),
                              static_cast<uintptr_t>(window_base + window_bytes), g_objects);
    }
    return ok;
}

}  // namespace

/* Before main, and before LLVM's own constructors: proves the runtime reached
 * this program's constructors at all. `debug_write` is safe this early (the
 * crt zeroes the TLS before the init array), so a run that prints nothing at
 * all hung before any of this program's code. */
__attribute__((constructor(1))) void cc_early_marker() noexcept
{
    aegir::debug_write("cc: runtime up\n");
}

/* The window is claimed here, at a priority above the library's own
 * constructors (which have the default priority, so they run last): a C++
 * program this size allocates from its global constructors, and before `init`
 * the memory syscalls answer -ENOSYS, so an allocation there fails. `seed_musl`
 * (200) has already pointed musl at the dispatcher; the window is what is
 * missing, and this gives it. */
__attribute__((constructor(300))) void cc_claim_heap() noexcept
{
    g_objects = new (g_objects_storage) aegir::mem::Allocator(nullptr);
    g_scratch = new (g_scratch_storage) aegir::mem::Scratch(nullptr);
    /* Room for LLVM's Support globals and a file mapping; the seed is a floor
     * and the heap grows past it through mem.main (specs/memory.md Phase 2). */
    constexpr uint64_t kHeapBytes = 16ull << 20;
    if (!adopt_memory()) {
        aegir::debug_write("  cc: FAIL no untyped, vspace or window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    if (!aegir::heap::init(*g_objects, *g_scratch, kHeapBytes)) {
        aegir::debug_write("  cc: FAIL the heap could not claim the window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
}

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("\naegir-cc: the on-device compiler's first measurement\n");

    /* The window was claimed by cc_claim_heap (constructor 300), before the
     * library's own constructors could allocate; a failure there halted the
     * process. */

    int failed = aegir::clang_probe::read_file("Initrd:services.manifest");
    aegir::debug_write(failed == 0 ? "AEGIR_CC_READ_OK\n" : "AEGIR_CC_READ_FAIL\n");

    /* The first real compile: clang's frontend and codegen, in this process,
     * writing an object to the writable volume (specs/clang-on-aegir.md). */
    int const compile_failed =
        aegir::clang_probe::compile("SCRATCH:CC_PROBE.C", "SCRATCH:CC_PROBE.O");
    aegir::debug_write(compile_failed == 0 ? "AEGIR_CC_COMPILE_OK\n"
                                           : "AEGIR_CC_COMPILE_FAIL\n");
    failed += compile_failed;

    /* The boot thread waits for this, so the marker can follow. */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    return failed;
}
