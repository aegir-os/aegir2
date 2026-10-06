/*
 * aegir-process-registry: the live processes, named by pid (specs/process.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * It owns `process.registry` and holds the live set: one Row per process, what
 * the spawn kit registered and what a Break sets. The policy beyond the
 * authority rule is the table's (aegir/process_table.h); this is the port that
 * serves it -- count, describe, break, register, unregister, owner
 * (aegir/process.h).
 *
 * The rows are the memory the manifest granted, mapped at the untyped's own
 * virtual address -- the shape aegir-vfs gives its table -- so the capacity is
 * the grant, not a number in the code (AGENTS.md: floors, never ceilings). The
 * same grant also holds one break-source slot per row (specs/process.md's Phase
 * 2) -- the capability a spawner minted from its child's notification, which
 * `break` signals -- and the release ports spawners named (Phase 3's halt). A
 * service given none holds no rows and refuses registration, loudly.
 *
 * It reports readiness through the supervision notification it was given
 * (specs/director.md) and never returns: a service that returns has stopped
 * being one, and its supervisor decides what that means.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/launch.h>
#include <aegir/mem/allocator.h>
#include <aegir/process.h>
#include <aegir/process_table.h>

#include <sel4/sel4.h>

namespace {

/* The slots the block did not name are ours, and a capability needs one to live
 * in; the run is adopted once, below, and slots come from its cursor. */
aegir::mem::Allocator g_slots(nullptr);

/* One spawner's release port: the `launch.session` caller half a spawner named,
 * keyed by the spawner's own badge -- the `parent` a row records -- so `break`
 * C can have the owner take a stuck process back (specs/process.md's Phase 3). */
struct Owner {
    uint64_t badge;
    seL4_CPtr port;
};

void write(char const *text) noexcept
{
    aegir::debug_write(text);
}

void write_word(uint64_t value) noexcept
{
    aegir::debug_write_unsigned(value);
}

/* Remember (or replace) the release port for `badge`. False when the badge is
 * zero or the owner table is full; a replaced port's old slot is deleted. */
bool set_owner(Owner *owners, uint32_t *count, uint32_t capacity, uint64_t badge,
               seL4_CPtr port) noexcept
{
    if (badge == 0) {
        return false;
    }
    for (uint32_t i = 0; i < *count; ++i) {
        if (owners[i].badge == badge) {
            seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, owners[i].port,
                              aegir::bootstrap::cnode_bits());
            owners[i].port = port;
            return true;
        }
    }
    if (*count >= capacity) {
        return false;
    }
    owners[*count].badge = badge;
    owners[*count].port = port;
    ++*count;
    return true;
}

/* Ask the owner of `owner_badge` to take `pid` back (specs/process.md's Phase
 * 3); false when no port was named, so C is a flag alone. Calling out is safe:
 * the owner's halt does not call back into this service. */
bool halt_owner(Owner const *owners, uint32_t count, uint64_t owner_badge,
                uint64_t pid) noexcept
{
    for (uint32_t i = 0; i < count; ++i) {
        if (owners[i].badge != owner_badge) {
            continue;
        }
        aegir::ipc::Consumer const owner(owners[i].port);
        uint64_t word = pid;
        uint64_t answer[1] = {0};
        (void)owner.call_words(aegir::launch::kMethodHalt, &word, 1, answer, 1);
        return true;
    }
    return false;
}

/* Remove a row and its source slot, mirroring the table's swap-on-remove so the
 * parallel array never drifts. Used for unregister and for the row a `break` C
 * leaves behind. */
void drop_row(aegir::process::ProcessTable &table, aegir::process::Row *rows,
              seL4_CPtr *sources, uint64_t pid) noexcept
{
    aegir::process::Row const *const row = table.find(pid);
    if (row == nullptr) {
        return;
    }
    uint32_t const index = static_cast<uint32_t>(row - rows);
    seL4_CPtr const source = sources[index];
    if (source != 0) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, source,
                          aegir::bootstrap::cnode_bits());
    }
    uint64_t word = pid;
    uint64_t answer[1] = {0};
    (void)table.handle(aegir::process::kMethodUnregister, &word, 1, 0, answer, 1);
    sources[index] = sources[table.count()];
    sources[table.count()] = 0;
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

    /* The backing store: the memory the manifest granted, already mapped at the
     * untyped's virtual address. It is one row, one source slot and one owner
     * record per process, so the capacity is the grant -- a starting pool, not a
     * ceiling. A service that asked for none keeps an empty table, and
     * registration refuses. */
    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    static_cast<void>(
        aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address));
    uint64_t const memory_bytes = memory_address == 0 ? 0 : (1ULL << memory_bits);
    uint64_t const per_row = sizeof(aegir::process::Row) + sizeof(seL4_CPtr) + sizeof(Owner);
    uint32_t const capacity =
        memory_bytes == 0 ? 0 : static_cast<uint32_t>(memory_bytes / per_row);
    auto *const rows = reinterpret_cast<aegir::process::Row *>(memory_address);
    auto *const sources = reinterpret_cast<seL4_CPtr *>(
        memory_address + static_cast<uint64_t>(capacity) * sizeof(aegir::process::Row));
    auto *const owners = reinterpret_cast<Owner *>(
        memory_address + static_cast<uint64_t>(capacity) * sizeof(aegir::process::Row) +
        static_cast<uint64_t>(capacity) * sizeof(seL4_CPtr));
    uint32_t owner_count = 0;
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

        /* A spawner names its release port once, so `break` C can halt its
         * children (specs/process.md's Phase 3). The call carries the port as
         * its one capability; the badge is the `parent` a row will record. */
        if (method == aegir::process::kMethodOwner) {
            bool named = false;
            if (count >= 1 && cap_arrived) {
                seL4_CPtr const slot = g_slots.alloc_slot();
                if (slot != 0 && aegir::ipc::take_received_cap(slot)) {
                    named = set_owner(owners, &owner_count, capacity, words[0], slot);
                    if (!named) {
                        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, slot,
                                          aegir::bootstrap::cnode_bits());
                    }
                } else if (slot != 0) {
                    aegir::ipc::drop_received_cap();
                }
            }
            if (!named && cap_arrived) {
                aegir::ipc::drop_received_cap();
            }
            if (named) {
                /* Say which spawner can now halt its children, so a boot shows
                 * the halt road being laid and a run can cue on it. */
                write("  process: owner named ");
                write_word(words[0]);
                write("\n");
            }
            uint64_t reply[1] = {named ? aegir::process::kOwnerNamed
                                       : aegir::process::kOwnerRefused};
            port.reply_words(reply, 1);
            continue;
        }

        /* Unregister is the table plus the source slot's return, in one place. */
        if (method == aegir::process::kMethodUnregister) {
            bool removed = false;
            if (count >= 1) {
                removed = table.find(words[0]) != nullptr;
                drop_row(table, rows, sources, words[0]);
            }
            if (cap_arrived) {
                aegir::ipc::drop_received_cap();
            }
            uint64_t reply[1] = {removed ? aegir::process::kProcessAdded
                                         : aegir::process::kProcessRefused};
            port.reply_words(reply, 1);
            continue;
        }

        uint64_t reply[aegir::ipc::kMaxWords];
        uint32_t const written =
            table.handle(method, words, count, badge, reply, aegir::ipc::kMaxWords);

        if (method == aegir::process::kMethodRegister) {
            if (written >= 1 && reply[0] == aegir::process::kProcessAdded &&
                count >= aegir::process::kRowWords) {
                /* The registration carries the child's break source as the
                 * call's one capability (specs/process.md): move it into a slot
                 * of its own and remember it against the row. No capability is a
                 * process the caller gave no way to wake. */
                uint64_t const pid =
                    reinterpret_cast<aegir::process::Row const *>(words)->pid;
                aegir::process::Row const *const row = table.find(pid);
                uint32_t const index =
                    row != nullptr ? static_cast<uint32_t>(row - rows) : 0;
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
                aegir::ipc::drop_received_cap();
            }
        } else if (cap_arrived) {
            /* A capability on a call that keeps none is cleared, so the next
             * transfer onto the scratch slot is not refused. */
            aegir::ipc::drop_received_cap();
        }

        if (method == aegir::process::kMethodBreak && written >= 1 &&
            reply[0] == aegir::process::kBreakSet && count >= 2) {
            /* C is the abort: signal the process's break source, which its
             * runtime waits on and exits on (specs/process.md's cooperative
             * half), and -- for a process that never returns to that wait -- ask
             * its owner to take it back (the halt). D, E and F are flags only.
             * Say what was broken, the way a registration is said. */
            uint64_t const pid = words[0];
            uint64_t const flags = words[1] & aegir::process::kAttnAll;
            aegir::process::Row const *const row = table.find(pid);
            if (row != nullptr && (flags & aegir::process::kAttnC) != 0) {
                seL4_CPtr const source = sources[static_cast<uint32_t>(row - rows)];
                if (source != 0) {
                    seL4_Signal(source);
                }
                bool const halted = halt_owner(owners, owner_count, row->parent, pid);
                if (halted) {
                    /* The process is taken back, so say it: a run can see the
                     * enforced halt land, and it is the cue a Break of a stuck
                     * process leaves. */
                    write("  process: halted ");
                    write_word(pid);
                    write("\n");
                    /* The owner's halt does not unregister -- a synchronous
                     * unregister would call back into this service while it
                     * waits on the halt call -- so the registry removes the row.
                     * With no owner named, C is only the flag and the source
                     * (and the spawner's own release will unregister), so the
                     * row stays until then. */
                    drop_row(table, rows, sources, pid);
                }
            }
            write("  process: break ");
            write_word(pid);
            write(" ");
            write_word(words[1]);
            write("\n");
        }

        port.reply_words(reply, written);
    }
}
