/*
 * aegir::command -- implementation. See command.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "aegir/command.h"

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/ipc/port.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/process.h>
#include <sel4/sel4.h>

namespace aegir::command {

namespace {

/* Static, like every hosted program's: the allocator's untyped table is tens
 * of kilobytes and a process's stack is pages (specs/userland.md). */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

bool adopt_memory() noexcept
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
        g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::cnode_bits()) - first_free, 0,
                          aegir::bootstrap::cnode_bits());
        ok = g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                             static_cast<uintptr_t>(window_base),
                             static_cast<uintptr_t>(window_base + window_bytes), &g_objects);
    }
    return ok;
}

/* The row's path: the program's directory, then its name, so it is the full
 * path when the spawner knew one (specs/process.md). Bounded by the field and
 * NUL-terminated. */
void compose_path(aegir::process::Row *row, char const *name) noexcept
{
    uint32_t dir_length = 0;
    char const *const dir = aegir::bootstrap::program_dir(&dir_length);
    uint32_t at = 0;
    if (dir != nullptr && dir_length > 0) {
        for (uint32_t i = 0; i < dir_length && at + 1 < aegir::process::kPathMax; ++i) {
            row->path[at++] = dir[i];
        }
        if (at > 0 && row->path[at - 1] != '/' && at + 1 < aegir::process::kPathMax) {
            row->path[at++] = '/';
        }
    }
    if (name != nullptr) {
        for (uint32_t i = 0; name[i] != '\0' && at + 1 < aegir::process::kPathMax; ++i) {
            row->path[at++] = name[i];
        }
    }
    row->path[at] = '\0';
}

/* Register this process with the process registry (specs/process.md), if the
 * command was handed a caller half: its pid (its own badge), the badge whose
 * class it runs as, its name and its path. Best-effort -- a command with no
 * registry capability runs unregistered, and a Break cannot name it. */
void register_self(char const *name) noexcept
{
    aegir::ipc::Consumer const registry = aegir::ipc::Consumer::find(
        aegir::process::kPortName, aegir::process::kPortNameLength);
    if (!registry.valid()) {
        return;
    }
    uint64_t badge = 0;
    if (!aegir::bootstrap::badge(&badge)) {
        return;
    }
    aegir::process::Row row{};
    row.pid = badge;
    /* The class is the check's subject (specs/process.md): the process's own
     * badge carries it, and the runtime knows no other. */
    row.owner = badge;
    compose_path(&row, name);
    if (name != nullptr) {
        uint32_t n = 0;
        for (; name[n] != '\0' && n + 1 < aegir::process::kNameMax; ++n) {
            row.name[n] = name[n];
        }
        row.name[n] = '\0';
    }
    uint64_t reply[1] = {0};
    (void)registry.call_words(aegir::process::kMethodRegister,
                              reinterpret_cast<uint64_t const *>(&row),
                              aegir::process::kRowWords, reply, 1);
}

}  // namespace

bool start(char const *name) noexcept
{
    if (!adopt_memory()) {
        aegir::debug_write("  ");
        aegir::debug_write(name != nullptr ? name : "command");
        aegir::debug_write(": FAIL no untyped, vspace or window\n");
        return false;
    }
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("  ");
        aegir::debug_write(name != nullptr ? name : "command");
        aegir::debug_write(": FAIL the heap would not claim the window\n");
        return false;
    }
    /* The runtime's one registration (specs/process.md): every command that
     * stood up here is in the live set, so a Break can name it. */
    register_self(name);
    return true;
}

}  // namespace aegir::command
