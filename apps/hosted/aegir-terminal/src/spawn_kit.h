/*
 * The terminal's spawn kit (specs/authority.md, specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Auth delegates the authority to start the session's commands; the terminal
 * uses it. A process has one VSpace root, so the terminal does not adopt a
 * window of its own: it builds its Spawner over the allocator and scratch the
 * toolkit already adopted, and adds the spawn untyped auth handed it. That is
 * the general shape a session's process uses to run a program (specs/
 * authority.md's "a terminal, bureau or launcher starts processes from it
 * without asking anyone").
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
    /* Adopt the delegated kit into the toolkit's allocator and build the
     * spawner. False when a grant is missing or an endpoint cannot be made;
     * the terminal then runs as it did before, with no external commands. */
    bool adopt(aegir::trinket::Application& app);

    bool ready() const { return ready_; }

    aegir::spawn::Spawner& spawner() { return *spawner_; }
    /* The endpoint the terminal serves con.stream on; a spawned command gets a
     * caller copy of it, badged with the stream it shares. */
    seL4_CPtr stream_endpoint() const { return stream_endpoint_; }
    /* Where a spawned command's faults arrive. Tier 1 does not read it. */
    seL4_CPtr fault_endpoint() const { return fault_endpoint_; }
    /* The unbadged copies the command's own log and namespace caps are minted
     * from (a badged endpoint cap cannot be minted again). */
    seL4_CPtr log_port() const { return log_port_; }
    seL4_CPtr nmspace_port() const { return nmspace_port_; }

private:
    aegir::mem::Account account_{"terminal-spawn", 0, 0, 0};
    std::unique_ptr<aegir::mem::Arena> arena_;
    std::unique_ptr<aegir::spawn::Initrd> initrd_;
    std::unique_ptr<aegir::spawn::Spawner> spawner_;
    seL4_CPtr stream_endpoint_ = 0;
    seL4_CPtr fault_endpoint_ = 0;
    seL4_CPtr log_port_ = 0;
    seL4_CPtr nmspace_port_ = 0;
    seL4_CPtr asid_pool_ = 0;
    bool ready_ = false;
};

}  // namespace aegir::terminal

#endif  // AEGIR_TERMINAL_SPAWN_KIT_H
