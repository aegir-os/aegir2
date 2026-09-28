/*
 * aegir-launcher: the session's launcher (specs/launch.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One launcher per session: the service that holds the spawn kit -- the
 * untyped, the ASID pool, the unbadged console.gui, mem.main, the namespace,
 * the log, the clock and timer -- and serves launch.session. The shell's
 * commands, the bureau's Execute, and later a dock and the desktop icons are
 * its clients; none of them holds the kit itself (specs/launch.md). It is
 * headless: it never draws, so it links no toolkit.
 *
 * auth starts it with the session and makes its endpoint: the owner half
 * arrives as the port `launch.session`, and the bureau and the terminal are
 * handed caller halves of the same endpoint, so no client depends on a name
 * the launcher chose (specs/authority.md).
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/launch.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/vspace.h>
#include <aegir/memory.h>
#include <aegir/nmspace.h>
#include <aegir/spawn/process.h>
#include <sel4/sel4.h>
#include <cstdint>

namespace {

void write(char const *text) noexcept
{
    aegir::debug_write("  launcher: ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

/* Static, like every service's: an allocator carries the tables of what it
 * handed out, and a service's stack is pages. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

/* The kit, adopted by name from the block auth installed: the unbadged sources
 * a child's own caps are minted from, and the session's namespace. */
struct Kit {
    seL4_CPtr log = 0;
    seL4_CPtr console_gui = 0;
    seL4_CPtr mem_main = 0;
    seL4_CPtr asid_pool = 0;
    seL4_CPtr clock = 0;
    seL4_CPtr timer = 0;
    seL4_CPtr nmspace = 0;
};
Kit g_kit;

bool adopt_kit() noexcept
{
    uint64_t slot = 0;
    if (!aegir::bootstrap::capability("spawn:log.main", 14, &slot)) return false;
    g_kit.log = static_cast<seL4_CPtr>(slot);
    if (!aegir::bootstrap::capability("spawn:console.gui", 17, &slot)) return false;
    g_kit.console_gui = static_cast<seL4_CPtr>(slot);
    if (!aegir::bootstrap::capability("spawn:mem.main", 14, &slot)) return false;
    g_kit.mem_main = static_cast<seL4_CPtr>(slot);
    if (!aegir::bootstrap::capability("asid-pool", 9, &slot)) return false;
    g_kit.asid_pool = static_cast<seL4_CPtr>(slot);
    if (!aegir::bootstrap::capability("shell:vfs.namespace", 19, &slot)) return false;
    g_kit.nmspace = static_cast<seL4_CPtr>(slot);
    /* The clock and timer are optional: a session without them still runs, and
     * only the time tools report it. */
    if (aegir::bootstrap::capability("spawn:clock.main", 16, &slot)) {
        g_kit.clock = static_cast<seL4_CPtr>(slot);
    }
    if (aegir::bootstrap::capability("spawn:timer.main", 16, &slot)) {
        g_kit.timer = static_cast<seL4_CPtr>(slot);
    }
    return true;
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    /* The endpoint auth made: the owner half of launch.session. */
    aegir::ipc::Owner port = aegir::ipc::Owner::find(aegir::launch::kPortName,
                                                    aegir::launch::kPortNameLength);
    if (!port.valid()) {
        write("FAIL no launch.session port to own");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The launcher's own memory: the untyped its objects and its children's
     * staging come from, and the window it maps the staging through. */
    uint64_t untyped_slot = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    if (!aegir::bootstrap::capability("untyped", 7, &untyped_slot) ||
        !aegir::bootstrap::capability("vspace", 6, &vspace_slot) ||
        !aegir::bootstrap::window(&window_base, &window_bytes)) {
        write("FAIL no memory and no address space were given");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    uint64_t untyped_physical = 0;
    uint32_t untyped_bits = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(
        aegir::bootstrap::untyped(&untyped_physical, &untyped_bits, &untyped_address));
    if (!g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                 untyped_physical)) {
        write("FAIL the memory I was given would not adopt");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
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
    g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::cnode_bits()) - first_free, 0);
    if (!g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                         static_cast<uintptr_t>(window_base),
                         static_cast<uintptr_t>(window_base + window_bytes), &g_objects)) {
        write("FAIL the window I was given could not be adopted");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    if (!adopt_kit()) {
        write("FAIL the spawn kit was not given");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    write("ready, serving launch.session");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    /* The launch protocol (aegir/launch.h). The launcher's own endpoint, so a
     * call and a reply are all it does; the spawning lands next. */
    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        seL4_Word badge = 0;
        uint32_t const method = port.receive_words(words, aegir::ipc::kMaxWords, &count, &badge);
        (void)badge;
        if (method == aegir::launch::kMethodSpawn ||
            method == aegir::launch::kMethodPipeline) {
            uint64_t const refused = 0;
            port.reply_words(&refused, 1);
            continue;
        }
        port.reply_words(nullptr, 0);
    }
}
