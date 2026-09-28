/*
 * The spawn service's kit (specs/authority.md, specs/shell.md, specs/memory.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Auth delegates the authority to start a session's programs; one process uses
 * it per session -- the launcher, or for the boot session the terminal itself.
 * This is that process's side of the spawn machinery, in one place so the
 * headless launcher and the terminal share it: the staged allocator over the
 * command chunks, the reserved CSpace pool one owner per live command, and the
 * reap that returns both whole (specs/memory.md Phase 5).
 *
 * It is deliberately toolkit-free. A hosted process has one VSpace root, and
 * the process that spawns stages through the window it was given -- for the
 * toolkit that is its own, for the headless launcher `g_scratch` -- so the
 * caller hands in the allocator and scratch rather than the kit reaching for a
 * singleton. What it does *not* build is the first-class grants of a child
 * (aegir/spawn/kit.h does that); this owns the memory and slots a spawn runs
 * on.
 */

#ifndef AEGIR_SPAWN_SERVICE_KIT_H
#define AEGIR_SPAWN_SERVICE_KIT_H

#include <aegir/ipc/port.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/slot_pool.h>
#include <aegir/spawn/initrd.h>
#include <aegir/spawn/kit.h>
#include <aegir/spawn/process.h>
#include <sel4/sel4.h>

#include <memory>
#include <vector>

namespace aegir::spawn {

class ServiceKit {
public:
    /* The untyped a command's own runtime is given at spawn: its heap and page
     * tables are retyped from it, and it is the command's *first* chunk, not
     * its ceiling -- the runtime asks mem.main for more as it grows
     * (specs/memory.md). */
    static constexpr uint32_t kCommandUntypedBits = 20; /* 1 MiB */

    /* Adopt the delegated kit, read by name from the bootstrap block.
     * `allocator` and `scratch` are the process's own -- the toolkit's, or a
     * headless service's `g_objects`/`g_scratch` -- and `slot_base`/
     * `slot_count` is the CSpace range reserved for commands (the toolkit
     * reserves everything above its own slots; a headless service reserves a
     * range of its own). False when a grant is missing or an endpoint cannot be
     * made; the caller then runs without a spawner. */
    bool adopt(aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch,
               uint64_t slot_base, uint32_t slot_count);

    bool ready() const { return ready_; }

    /* Stage one command's spawn, in the pool slots owned by `owner` (an id the
     * caller picks, one per live command): reset the allocator, point it at
     * that owner, and build the spawner. begin_command() then mints the memory
     * copy the command's chunks are owned by, and end_staging() drops the
     * staging and rewinds the window -- the command is alive, its capabilities
     * are in the pool, and the next command may be staged at once. */
    bool begin(uint32_t owner);
    bool begin_command(uint64_t badge);
    void end_staging();

    /* A command's staging that never produced a live command (a missing
     * image, a spawn that failed): release whatever its badge owns and return
     * its slots, then drop the staging. */
    void abandon(uint64_t badge, uint32_t owner);

    /* Rewind the staging window to where it stood before the first live
     * command was staged. Valid only when no command is live: the revoke that
     * reaped them unmapped their staging frames, and this is bookkeeping
     * (specs/memory.md Phase 5). */
    void rewind_staging();

    /* Stop and reclaim one command: suspend its TCB, release its memory by
     * badge -- the chunks' capabilities, the TCB among them, go with it -- and
     * return its pool slots. `tcb` may be zero when the command never ran. */
    void reap(seL4_CPtr tcb, uint64_t badge, uint32_t owner);

    Spawner& spawner() { return *spawner_; }
    /* The allocator over the command chunks: the spawner's objects and a
     * command's seed are retyped from it, and its untyped source is mem.main
     * (specs/memory.md). */
    aegir::mem::Allocator& memory();

    /* The first-class kit (specs/launch.md): the capabilities every child's
     * grant is built from. The caller builds a command's, a nested terminal's
     * and the shell's ports with aegir::spawn::command_ports / launcher_ports /
     * shell_ports over this, so the lists live in one place. It is mutable
     * because a launcher sets `stream` per request to the caller's own.
     */
    Kit& kit() { return kit_; }
    Kit const& kit() const { return kit_; }

    /* The current command's memory copy: minted from mem_port_ and badged with
     * the command's id, so the service records its chunks as that command's.
     * A copy of it goes to the command, so its own runtime grows within the
     * same ownership. Dropped by end_staging. */
    seL4_CPtr command_mem() const { return command_mem_; }

    /* Where a command's faults arrive. Tier 1 does not read it. */
    seL4_CPtr fault_endpoint() const { return fault_endpoint_; }

    /* The shell process: spawned once from auth's `shell-pool`, not pooled and
     * reclaimed like a command, because it lives as long as its terminal. It
     * runs on `badge` -- the stream key its con.stream copy carries -- and the
     * terminal serves it like any other client. `arguments` are what follow
     * argv[0] (specs/environment.md): for the boot session, the command file
     * the shell is to run. */
    bool spawn_shell(char const *image, uint64_t image_bytes, char const *cwd,
                     uint32_t cwd_length, uint64_t badge, char const *const *arguments,
                     uint32_t argument_count);

    /* The launcher kit a child is built from: true when an unbadged console.gui
     * was delegated, so a nested terminal can mint its own. */
    bool can_launch() const { return kit_.console_gui != 0; }

    /* The boot session's status endpoint, when this process is the boot
     * session's (auth grants it as `boot.status`): the shell sends the outcome
     * -- 0 success, nonzero failure -- and auth receives it (specs/boot.md).
     * Zero for an interactive session. */
    seL4_CPtr boot_status() const { return boot_status_; }

private:
    aegir::mem::Allocator *allocator_ = nullptr;
    aegir::mem::Scratch *scratch_ = nullptr;
    uint64_t slot_base_ = 0;
    uint32_t slot_count_ = 0;
    aegir::mem::Account account_{"spawn-service", 0, 0, 0};
    std::unique_ptr<aegir::mem::Arena> arena_;
    std::unique_ptr<aegir::spawn::Initrd> initrd_;
    std::unique_ptr<aegir::spawn::Spawner> spawner_;
    /* The reserved command-slot pool: one owner per live command, so reaping
     * one returns only its slots (specs/memory.md Phase 5). */
    aegir::mem::SlotPool slot_pool_;
    std::vector<uint32_t> slot_owners_;
    seL4_CPtr stream_endpoint_ = 0;
    seL4_CPtr fault_endpoint_ = 0;
    seL4_CPtr command_doorbell_ = 0;
    seL4_CPtr log_port_ = 0;
    seL4_CPtr nmspace_port_ = 0;
    seL4_CPtr command_nmspace_port_ = 0;
    seL4_CPtr command_clock_port_ = 0;
    seL4_CPtr command_timer_port_ = 0;
    seL4_CPtr boot_status_ = 0;
    seL4_CPtr asid_pool_ = 0;
    /* The unbadged mem.main copy, and the per-command copy minted from it
     * (specs/memory.md). The command copy lives in one toolkit slot, re-minted
     * for each command while it is staged, and dropped once it is spawned --
     * the command holds its own copy. */
    seL4_CPtr mem_port_ = 0;
    seL4_CPtr command_mem_ = 0;
    bool command_mem_live_ = false;
    /* The first-class kit every child's grant is built from (specs/launch.md). */
    aegir::spawn::Kit kit_{};
    seL4_CPtr shell_pool_ = 0;
    uint32_t shell_pool_bits_ = 0;
    uintptr_t scratch_mark_ = 0;
    bool staged_since_rewind_ = false;
    bool ready_ = false;
};

}  // namespace aegir::spawn

#endif  // AEGIR_SPAWN_SERVICE_KIT_H
