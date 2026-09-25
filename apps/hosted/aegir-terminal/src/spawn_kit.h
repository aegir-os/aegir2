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

namespace aegir::trinket {
class Application;
}

namespace aegir::terminal {

class SpawnKit {
public:
    /* The untyped a command's own runtime is given: its heap and page tables
     * are retyped from it, and it is carved from the command pool so the
     * command's exit reclaims it. */
    static constexpr uint32_t kCommandUntypedBits = 20; /* 1 MiB */

    /* Adopt the delegated kit. False when a grant is missing or an endpoint
     * cannot be made; the terminal then runs as it did before. */
    bool adopt(aegir::trinket::Application& app);

    bool ready() const { return ready_; }

    /* Bracket a command: begin() re-adopts the pool and the reserved slots and
     * builds a spawner; finish()/abort() suspend (when there is a command),
     * revoke the pool, release the slots and drop the staging. */
    bool begin();
    void finish(seL4_CPtr tcb);
    void abort();

    aegir::spawn::Spawner& spawner() { return *spawner_; }
    /* The allocator over the command pool: a command's own runtime untyped is
     * carved from it, so the command's exit reclaims that too. */
    aegir::mem::Allocator& memory();

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

    /* The shell process: spawned once from auth's `shell-pool`, not bracketed
     * and reclaimed like a command, because it lives as long as the terminal.
     * It runs on `badge` -- the stream key its con.stream copy carries -- and
     * the terminal serves it like any other client. */
    bool spawn_shell(char const *image, uint64_t image_bytes, char const *cwd,
                     uint32_t cwd_length, uint64_t badge);

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
    seL4_CPtr asid_pool_ = 0;
    seL4_CPtr command_pool_ = 0;
    uint32_t command_pool_bits_ = 0;
    seL4_CPtr shell_pool_ = 0;
    uint32_t shell_pool_bits_ = 0;
    uintptr_t scratch_mark_ = 0;
    bool ready_ = false;
};

}  // namespace aegir::terminal

#endif  // AEGIR_TERMINAL_SPAWN_KIT_H