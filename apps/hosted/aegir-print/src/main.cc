/*
 * aegir-print: a hosted command (specs/shell.md's Phase 4).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Where aegir-echo speaks Aegir's console API directly, this command uses the
 * hosted runtime and nothing else: `printf` reaches the grid because the
 * runtime routes fd 1/2 to the console stream, and `return` reports the
 * status because the runtime's exit carries it. It is what a session's command
 * becomes once the stream is underneath libc rather than beside it.
 *
 * It stands the runtime up (the untyped, VSpace root and window the terminal's
 * spawn kit gives it) and prints its arguments. A numeric first argument is
 * its exit status, so the shell's return-code line has something to print.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/environment.h>
#include <aegir/heap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <cstdio>
#include <cstdlib>

namespace {

/* Static, like every hosted smoke's: the allocator's tables are tens of
 * kilobytes and a service's stack is pages. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

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
        g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::kCNodeBits) - first_free, 0);
        ok = g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                             static_cast<uintptr_t>(window_base),
                             static_cast<uintptr_t>(window_base + window_bytes), &g_objects);
    }
    return ok;
}

/* The first argument as a decimal status, or zero when it is not a number. */
uint64_t status_from(char const *text)
{
    if (text[0] == '\0') {
        return 0;
    }
    uint64_t value = 0;
    for (uint32_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    return value;
}

}  // namespace

int main(int argc, char **argv)
{
    if (!adopt_memory()) {
        aegir::debug_write("  aegir-print: FAIL no untyped, vspace or window\n");
        std::_Exit(127);
    }
    constexpr uint64_t kHeapBytes = 8ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("  aegir-print: FAIL the heap would not claim the window\n");
        std::_Exit(127);
    }

    for (int i = 0; i < argc; ++i) {
        std::printf(i == 0 ? "%s" : " %s", argv[i]);
    }
    std::printf("\n");
    std::fflush(stdout);

    /* A plain return carries the status: the hosted runtime's exit callback
     * reports it through the console stream, the same path `std::exit` takes
     * (specs/shell.md's Phase 4). A numeric argument is the status; without
     * one, the inherited variable exitcode is, so a shell's `Set` is visible
     * here -- the environment the shell passes on (specs/shell.md's Phase 5). */
    uint64_t status = 0;
    if (argc > 1) {
        status = status_from(argv[1]);
    } else if (char const *const from_environment =
                   aegir::environment::getenv("exitcode")) {
        status = status_from(from_environment);
    }
    return static_cast<int>(status);
}