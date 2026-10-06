/*
 * aegir-process-registry: the live processes, named by pid (specs/process.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * It owns `process.registry` and holds the live set: one Row per process, what
 * the spawn kit registered and what a Break sets. The policy beyond the
 * authority rule is the table's (aegir/process_table.h); this is the port that
 * serves it -- count, describe, break, register, unregister (aegir/process.h).
 *
 * The rows are the memory the manifest granted, mapped at the untyped's own
 * virtual address -- the shape aegir-vfs gives its table -- so the capacity is
 * the grant, not a number in the code (AGENTS.md: floors, never ceilings). A
 * service given none holds no rows and refuses registration, loudly.
 *
 * It reports readiness through the supervision notification it was given
 * (specs/director.md) and never returns: a service that returns has stopped
 * being one, and its supervisor decides what that means.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/process.h>
#include <aegir/process_table.h>

#include <sel4/sel4.h>

namespace {

void write(char const *text) noexcept
{
    aegir::debug_write(text);
}

void write_word(uint64_t value) noexcept
{
    aegir::debug_write_unsigned(value);
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Owner port = aegir::ipc::Owner::find(aegir::process::kPortName,
                                                     aegir::process::kPortNameLength);
    if (!port.valid()) {
        write("  process: no process.registry port: nothing to own\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The table's backing store: the memory the manifest granted, already
     * mapped at the untyped's virtual address. A service that asked for none
     * keeps an empty table, and registration refuses. */
    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    static_cast<void>(
        aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address));
    auto *const rows = reinterpret_cast<aegir::process::Row *>(memory_address);
    uint32_t const capacity =
        memory_address == 0
            ? 0
            : static_cast<uint32_t>((1ULL << memory_bits) / sizeof(aegir::process::Row));
    aegir::process::ProcessTable table(rows, capacity);

    write("  process: process.registry ready, ");
    write_word(capacity);
    write(" rows\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        seL4_Word badge = 0;
        uint32_t const method =
            port.receive_words(words, aegir::ipc::kMaxWords, &count, &badge);
        uint64_t reply[aegir::ipc::kMaxWords];
        uint32_t const written =
            table.handle(method, words, count, badge, reply, aegir::ipc::kMaxWords);
        if (method == aegir::process::kMethodRegister && written >= 1 &&
            reply[0] == aegir::process::kProcessAdded &&
            count >= aegir::process::kRowWords) {
            /* Say what joined, so a boot shows the live set forming (and the
             * acceptance has a line to cue on). The stored row's name is the
             * NUL-terminated one, not the raw request's. */
            uint64_t const pid =
                reinterpret_cast<aegir::process::Row const *>(words)->pid;
            aegir::process::Row const *const row = table.find(pid);
            write("  process: registered ");
            write_word(pid);
            write(" ");
            write(row != nullptr ? row->name : "?");
            write("\n");
        }
        port.reply_words(reply, written);
    }
}
