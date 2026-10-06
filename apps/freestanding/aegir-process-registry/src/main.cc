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
 * the grant, not a number in the code (AGENTS.md: floors, never ceilings). The
 * same grant is split into the rows and one break-source slot per row
 * (specs/process.md's Phase 2): the capability a spawner minted from its child's
 * notification, which `break` signals. A service given none holds no rows and
 * refuses registration, loudly.
 *
 * It reports readiness through the supervision notification it was given
 * (specs/director.md) and never returns: a service that returns has stopped
 * being one, and its supervisor decides what that means.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/mem/allocator.h>
#include <aegir/process.h>
#include <aegir/process_table.h>

#include <sel4/sel4.h>

namespace {

/* The slots the block did not name are ours, and a source capability needs one
 * to live in; the run is adopted once, below, and slots come from its cursor. */
aegir::mem::Allocator g_slots(nullptr);

constexpr uint32_t kNoIndex = 0xFFFFFFFFu;

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
     * mapped at the untyped's virtual address. It is one row and one source slot
     * per process, so the capacity is the grant -- a starting pool, not a
     * ceiling. A service that asked for none keeps an empty table, and
     * registration refuses. */
    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    static_cast<void>(
        aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address));
    uint64_t const memory_bytes = memory_address == 0 ? 0 : (1ULL << memory_bits);
    uint32_t const capacity =
        memory_bytes == 0
            ? 0
            : static_cast<uint32_t>(memory_bytes /
                                    (sizeof(aegir::process::Row) + sizeof(seL4_CPtr)));
    auto *const rows = reinterpret_cast<aegir::process::Row *>(memory_address);
    auto *const sources = reinterpret_cast<seL4_CPtr *>(
        memory_address + static_cast<uint64_t>(capacity) * sizeof(aegir::process::Row));
    aegir::process::ProcessTable table(rows, capacity);

    /* What the block names is ours; every slot past it is ours to use, the same
     * adoption every service with an untyped walks. */
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind == aegir::bootstrap::EntryKind::Capability &&
                entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            } else if (entry.kind == aegir::bootstrap::EntryKind::DeviceCapability &&
                       entry.reserved + 1 > first_free) {
                first_free = entry.reserved + 1;
            }
        }
    }
    g_slots.adopt_slots(first_free,
                        (1u << aegir::bootstrap::cnode_bits()) - first_free, 0,
                        aegir::bootstrap::cnode_bits());

    write("  process: process.registry ready, ");
    write_word(capacity);
    write(" rows\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        seL4_Word badge = 0;
        bool cap_arrived = false;
        uint32_t const method = port.receive_words(words, aegir::ipc::kMaxWords, &count,
                                                   &badge, &cap_arrived);

        /* An unregister's row index has to be read before the table moves the
         * last live row into the hole: the source slot array moves with it. */
        uint32_t removed_index = kNoIndex;
        if (method == aegir::process::kMethodUnregister && count >= 1) {
            aegir::process::Row const *const row = table.find(words[0]);
            if (row != nullptr) {
                removed_index = static_cast<uint32_t>(row - rows);
            }
        }

        uint64_t reply[aegir::ipc::kMaxWords];
        uint32_t const written =
            table.handle(method, words, count, badge, reply, aegir::ipc::kMaxWords);

        if (method == aegir::process::kMethodRegister && written >= 1 &&
            reply[0] == aegir::process::kProcessAdded &&
            count >= aegir::process::kRowWords) {
            /* The registration carries the child's break source as the call's
             * one capability (specs/process.md): move it into a slot of its own
             * and remember it against the row. No capability is a process the
             * caller gave no way to wake -- the halt (Phase 3) still reaches it.
             */
            uint64_t const pid = reinterpret_cast<aegir::process::Row const *>(words)->pid;
            aegir::process::Row const *const row = table.find(pid);
            uint32_t const index = row != nullptr ? static_cast<uint32_t>(row - rows) : 0;
            seL4_CPtr source = 0;
            if (cap_arrived) {
                seL4_CPtr const slot = g_slots.alloc_slot();
                if (slot != 0 && aegir::ipc::take_received_cap(slot)) {
                    source = slot;
                } else {
                    aegir::ipc::drop_received_cap();
                }
            }
            sources[index] = source;
            /* Say what joined, so a boot shows the live set forming (and the
             * acceptance has a line to cue on). The stored row's name is the
             * NUL-terminated one, not the raw request's. */
            write("  process: registered ");
            write_word(pid);
            write(" ");
            write(row != nullptr ? row->name : "?");
            write("\n");
        } else if (cap_arrived) {
            /* A capability on a call that keeps none is cleared, so the next
             * transfer onto the scratch slot is not refused. */
            aegir::ipc::drop_received_cap();
        }

        if (method == aegir::process::kMethodBreak && written >= 1 &&
            reply[0] == aegir::process::kBreakSet && count >= 2) {
            /* C is the abort: signal the process's break source, which its
             * runtime waits on and exits on (specs/process.md). D, E and F are
             * flags only -- set in the row, waking nothing. Say what was broken,
             * the way a registration is said. */
            uint64_t const pid = words[0];
            uint64_t const flags = words[1] & aegir::process::kAttnAll;
            aegir::process::Row const *const row = table.find(pid);
            if (row != nullptr && (flags & aegir::process::kAttnC) != 0) {
                seL4_CPtr const source = sources[static_cast<uint32_t>(row - rows)];
                if (source != 0) {
                    seL4_Signal(source);
                }
            }
            write("  process: break ");
            write_word(pid);
            write(" ");
            write_word(words[1]);
            write("\n");
        }

        if (method == aegir::process::kMethodUnregister && written >= 1 &&
            reply[0] == aegir::process::kProcessAdded && removed_index != kNoIndex) {
            /* Release the source slot and move the last live row's into the hole,
             * mirroring the table's swap. */
            seL4_CPtr const source = sources[removed_index];
            if (source != 0) {
                seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, source,
                                  aegir::bootstrap::cnode_bits());
            }
            sources[removed_index] = sources[table.count()];
            sources[table.count()] = 0;
        }

        port.reply_words(reply, written);
    }
}
