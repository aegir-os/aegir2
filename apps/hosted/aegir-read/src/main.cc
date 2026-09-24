/*
 * aegir-read: a hosted command that reads the console (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The command that proves fd 0: the shell hands it the console stream, and
 * while it runs the terminal routes the keyboard to that stream's input queue
 * (specs/terminal.md, specs/shell.md's Phase 4). It reads fd 0 -- the hosted
 * runtime's poll of the stream -- echoes every byte to fd 1, and exits 0 when
 * a line ends. `read` answering zero is the poll's "nothing queued yet", not
 * EOF, so it asks again; each ask is an IPC to the terminal, which is what
 * lets the terminal service the keyboard between reads.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <cstdlib>
#include <unistd.h>

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

}  // namespace

int main(int argc, char **argv)
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    if (!adopt_memory()) {
        aegir::debug_write("  aegir-read: FAIL no untyped, vspace or window\n");
        std::_Exit(127);
    }
    constexpr uint64_t kHeapBytes = 8ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("  aegir-read: FAIL the heap would not claim the window\n");
        std::_Exit(127);
    }

    char buffer[256];
    for (;;) {
        long const got = ::read(0, buffer, sizeof(buffer));
        if (got <= 0) {
            /* The poll found nothing queued; ask again. Each ask is an IPC to
             * the terminal, so the terminal still runs and services the
             * keyboard between reads. */
            continue;
        }
        (void)::write(1, buffer, static_cast<size_t>(got));
        for (long i = 0; i < got; ++i) {
            if (buffer[i] == '\n' || buffer[i] == '\r') {
                /* A hosted program just returns: the runtime's exit callback
                 * reports the status through the console stream. */
                return 0;
            }
        }
    }
}