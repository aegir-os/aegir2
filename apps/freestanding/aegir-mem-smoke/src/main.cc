/*
 * aegir-mem-smoke: the memory service's client (specs/memory.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The client half of Phase 1: ask mem.main for a chunk, take the capability it
 * rides back, retype a frame from it -- which is what a runtime grows with --
 * and release, so the whole protocol is exercised by a caller and not by the
 * service against itself.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/memory.h>

#include <sel4/sel4.h>

namespace {

void write(char const *text) noexcept
{
    aegir::debug_write(text);
}

/* The first slot past everything the bootstrap block named: the smoke's own. */
uint64_t first_free_slot() noexcept
{
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
    return first_free;
}

void fail(char const *what) noexcept
{
    /* The harness counts a smoke's own failure by this marker
     * (scripts/run_target.py's GUEST_FAILURE): the readable name first, the
     * marker it looks for, then what. */
    write("  mem-smoke: MEM_SMOKE_FAIL ");
    write(what);
    write("\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    aegir::ipc::Consumer const mem = aegir::ipc::Consumer::find(
        aegir::memory::kPortName, aegir::memory::kPortNameLength);
    if (!mem.valid()) {
        fail("no mem.main");
    }

    /* Ask for a chunk. */
    uint64_t const request = aegir::memory::kChunkBits;
    /* Two words: the chunk's size, then the chunk's physical address. */
    uint64_t answer[2] = {};
    bool cap_arrived = false;
    aegir::ipc::WordsReply const alloc =
        mem.call_transfer(aegir::memory::kMethodAlloc, &request, 1, 0, answer, 2,
                          &cap_arrived);
    if (alloc.error != 0 || alloc.count < 1 || !cap_arrived ||
        answer[0] != aegir::memory::kChunkBits) {
        fail("alloc answered no chunk");
    }

    uint64_t const chunk_slot = first_free_slot();
    if (!aegir::ipc::take_received_cap(static_cast<seL4_CPtr>(chunk_slot))) {
        fail("the chunk capability did not move");
    }

    /* Grow in it, as a runtime does: retype a frame from the chunk. */
    uint64_t const frame_slot = chunk_slot + 1;
    seL4_Error const retyped = seL4_Untyped_Retype(
        static_cast<seL4_CPtr>(chunk_slot), seL4_RISCV_4K_Page, seL4_PageBits,
        aegir::bootstrap::kSlotOwnCNode, aegir::bootstrap::kSlotOwnCNode, 0,
        static_cast<seL4_CPtr>(frame_slot), 1);
    if (retyped != seL4_NoError) {
        fail("the chunk would not retype a frame");
    }

    /* Release the caller's own chunks (badge 0): the frame goes with it. */
    uint64_t const own = 0;
    uint64_t released = 0;
    aegir::ipc::WordsReply const answer2 =
        mem.call_words(aegir::memory::kMethodRelease, &own, 1, &released, 1);
    if (answer2.error != 0 || answer2.count != 1 || released < 1) {
        fail("release did not take the chunk back");
    }

    write("  mem-smoke: ok, a chunk came, retyped a frame, and released\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
