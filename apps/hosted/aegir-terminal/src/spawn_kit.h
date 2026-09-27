/*
 * The terminal's spawn kit (specs/authority.md, specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Auth delegates the authority to start the session's commands; the terminal
 * uses it. A process has one VSpace root, so the terminal does not adopt a
 * window of its own: its spawner stages through the toolkit's window. But the
 * commands run *out* of a pool of their own, and their capabilities live in a
 * CSpace sub-range the toolkit reserves -- because a long-lived spawner must
 * reclaim a command when it exits, and reclaim is one revoke of the pool plus
 * one release of the slots, which the toolkit's own allocator cannot give
 * (specs/shell.md's Phase 4). So a command is bracketed by begin()/finish().
 */

#ifndef AEGIR_TERMINAL_SPAWN_KIT_H
#define AEGIR_TERMINAL_SPAWN_KIT_H

#include <aegir/ipc/port.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/spawn/initrd.h>
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

    /* Bracket the commands of one line: begin() re-adopts the reserved slots
     * and builds a spawner; begin_command() mints the memory copy a command's
     * chunks are owned by, called once per stage before it is spawned;
     * finish_all() suspends the commands (a pipeline may have several),
     * releases each command's memory back to the service, releases the slots
     * and drops the staging. */
    bool begin();
    bool begin_command(uint64_t badge);
    void finish_all(std::vector<aegir::spawn::Process> const &processes);
    void abort();

    aegir::spawn::Spawner& spawner() { return *spawner_; }
    /* The allocator over the command chunks: the spawner's objects and a
     * command's seed are retyped from it, and its untyped source is mem.main
     * (specs/memory.md). */
    aegir::mem::Allocator& memory();

    /* The unbadged mem.main copy, for the terminal's release calls. */
    seL4_CPtr mem_port() const { return mem_port_; }
    /* The current command's memory copy: minted from mem_port_ and badged with
     * the command's id, so the service records its chunks as that command's.
     * A copy of it goes to the command, so its own runtime grows within the
     * same ownership. */
    seL4_CPtr command_mem() const { return command_mem_; }

    /* The endpoint the terminal serves con.stream on; a command gets a caller
     * copy of it, badged with the stream it shares. */
    seL4_CPtr stream_endpoint() const { return stream_endpoint_; }
    /* Where a command's faults arrive. Tier 1 does not read it. */
    seL4_CPtr fault_endpoint() const { return fault_endpoint_; }
    /* The unbadged copies a command's own caps are minted from. */
    seL4_CPtr log_port() const { return log_port_; }
    seL4_CPtr nmspace_port() const { return nmspace_port_; }
    /* The terminal's own badged namespace, which a command is handed by *copy*
     * (specs/dos.md): it carries the session's identity, so a command resolves
     * Home:/ENV:/C: and its writes are owned by the session, without a command
     * naming a slot or the terminal knowing a badge value. */
    seL4_CPtr command_nmspace_port() const { return command_nmspace_port_; }

    /* The notification the terminal rings when a running command's stream has
     * input, so a command's `read` can park instead of poll (specs/terminal.md).
     * One for the command pool: only one command runs at a time. A copy is
     * granted to each command as `con.doorbell`. */
    seL4_CPtr command_doorbell() const { return command_doorbell_; }

    /* The unbadged clock the shell and each command are handed as clock.main,
     * so the runtime's clock_gettime answers for the DOS tools and the shell's
     * Date/Time (specs/dos.md). Zero when auth was given no clock. */
    seL4_CPtr command_clock_port() const { return command_clock_port_; }

    /* The unbadged timer the shell and each command are handed as timer.main,
     * so the runtime's nanosleep and CLOCK_MONOTONIC answer (specs/timer.md).
     * Zero when auth was given no timer. */
    seL4_CPtr command_timer_port() const { return command_timer_port_; }

    /* The shell process: spawned once from auth's `shell-pool`, not bracketed
     * and reclaimed like a command, because it lives as long as the terminal.
     * It runs on `badge` -- the stream key its con.stream copy carries -- and
     * the terminal serves it like any other client. `arguments` are what
     * follow argv[0] (specs/environment.md): for the boot session, the command
     * file the shell is to run. */
    bool spawn_shell(char const *image, uint64_t image_bytes, char const *cwd,
                     uint32_t cwd_length, uint64_t badge, char const *const *arguments,
                     uint32_t argument_count);

    /* The boot session's status endpoint, when this terminal is the boot
     * session's (auth grants it as `boot.status`): the shell sends the outcome
     * -- 0 success, nonzero failure -- and auth receives it (specs/boot.md).
     * Zero for an interactive terminal. */
    seL4_CPtr boot_status() const { return boot_status_; }

private:
    void reclaim();

    aegir::trinket::Application* app_ = nullptr;
    aegir::mem::Account account_{"terminal-spawn", 0, 0, 0};
    std::unique_ptr<aegir::mem::Arena> arena_;
    std::unique_ptr<aegir::spawn::Initrd> initrd_;
    std::unique_ptr<aegir::spawn::Spawner> spawner_;
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
     * (specs/memory.md Phase 3). The command copy lives in one slot, re-minted
     * for each stage; command_badges_ is what reclaim releases. */
    seL4_CPtr mem_port_ = 0;
    seL4_CPtr command_mem_ = 0;
    bool command_mem_live_ = false;
    std::vector<uint64_t> command_badges_;
    seL4_CPtr shell_pool_ = 0;
    uint32_t shell_pool_bits_ = 0;
    uintptr_t scratch_mark_ = 0;
    bool ready_ = false;
};

}  // namespace aegir::terminal

#endif  // AEGIR_TERMINAL_SPAWN_KIT_H