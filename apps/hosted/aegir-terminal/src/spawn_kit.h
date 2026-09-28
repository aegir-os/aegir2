/*
 * The terminal's spawn kit (specs/authority.md, specs/shell.md, specs/memory.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Auth delegates the authority to start the session's commands; the terminal
 * uses it. A process has one VSpace root, so the terminal does not adopt a
 * window of its own: its spawner stages through the toolkit's window. The
 * commands' capabilities live in a CSpace sub-range the toolkit reserves, and
 * that range is a *pool*: each command owns the slots it was built from, and
 * reaping it -- one release of its memory chunks, which deletes those
 * capabilities, then one return of its slots -- makes it whole again
 * (specs/memory.md Phase 5). That is what lets a `Run` command keep running
 * while the shell starts the next one: a background command is one more owner
 * of the pool, not a second bracket the terminal has to serialize.
 */

#ifndef AEGIR_TERMINAL_SPAWN_KIT_H
#define AEGIR_TERMINAL_SPAWN_KIT_H

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

namespace aegir::trinket {
class Application;
}

namespace aegir::terminal {

class SpawnKit {
public:
    /* The untyped a command's own runtime is given at spawn: its heap and page
     * tables are retyped from it, and it is the command's *first* chunk, not
     * its ceiling -- the runtime asks mem.main for more as it grows
     * (specs/memory.md). */
    static constexpr uint32_t kCommandUntypedBits = 20; /* 1 MiB */

    /* Adopt the delegated kit. False when a grant is missing or an endpoint
     * cannot be made; the terminal then runs as it did before. */
    bool adopt(aegir::trinket::Application& app);

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

    aegir::spawn::Spawner& spawner() { return *spawner_; }
    /* The allocator over the command chunks: the spawner's objects and a
     * command's seed are retyped from it, and its untyped source is mem.main
     * (specs/memory.md). */
    aegir::mem::Allocator& memory();

    /* The first-class kit (specs/launch.md): the capabilities every child's
     * grant is built from. main builds a command's, a nested terminal's and the
     * shell's ports with aegir::spawn::command_ports / launcher_ports /
     * shell_ports over this, so the lists live in one place. */
    aegir::spawn::Kit const& kit() const { return kit_; }

    /* The current command's memory copy: minted from mem_port_ and badged with
     * the command's id, so the service records its chunks as that command's.
     * A copy of it goes to the command, so its own runtime grows within the
     * same ownership. Dropped by end_staging. */
    seL4_CPtr command_mem() const { return command_mem_; }

    /* Where a command's faults arrive. Tier 1 does not read it. */
    seL4_CPtr fault_endpoint() const { return fault_endpoint_; }

    /* The shell process: spawned once from auth's `shell-pool`, not pooled and
     * reclaimed like a command, because it lives as long as the terminal. It
     * runs on `badge` -- the stream key its con.stream copy carries -- and the
     * terminal serves it like any other client. `arguments` are what follow
     * argv[0] (specs/environment.md): for the boot session, the command file
     * the shell is to run. */
    bool spawn_shell(char const *image, uint64_t image_bytes, char const *cwd,
                     uint32_t cwd_length, uint64_t badge, char const *const *arguments,
                     uint32_t argument_count);

    /* The launcher kit a nested terminal is built from (specs/launch.md): the
     * unbadged console.gui it mints the child's own from. Its memory comes
     * from mem.main on demand, under the terminal's badge, not from a pool. */
    bool can_launch() const { return spawn_console_gui_ != 0; }

    /* The boot session's status endpoint, when this terminal is the boot
     * session's (auth grants it as `boot.status`): the shell sends the outcome
     * -- 0 success, nonzero failure -- and auth receives it (specs/boot.md).
     * Zero for an interactive terminal. */
    seL4_CPtr boot_status() const { return boot_status_; }

private:
    aegir::trinket::Application* app_ = nullptr;
    aegir::mem::Account account_{"terminal-spawn", 0, 0, 0};
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
    /* The launcher kit (specs/launch.md): the unbadged console.gui a nested
     * terminal's own is minted from. */
    seL4_CPtr spawn_console_gui_ = 0;
    /* The first-class kit every child's grant is built from (specs/launch.md). */
    aegir::spawn::Kit kit_{};
    seL4_CPtr shell_pool_ = 0;
    uint32_t shell_pool_bits_ = 0;
    uintptr_t scratch_mark_ = 0;
    bool staged_since_rewind_ = false;
    bool ready_ = false;
};

}  // namespace aegir::terminal

#endif  // AEGIR_TERMINAL_SPAWN_KIT_H
