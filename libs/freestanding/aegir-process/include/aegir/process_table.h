/*
 * The process registry's table: the live set and the authority check
 * (specs/process.md), apart from the kernel so the host can test it.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The service (aegir-process-registry) owns storage and the IPC loop; this is
 * the logic -- which pids are live, what a row says, who may break whom. It
 * knows no seL4, so scripts/check_process.py drives it on the host exactly as
 * the service drives it on the machine.
 *
 * The table does not own its rows: the caller hands it a run of them and grows
 * that run when it must, so nothing here is a ceiling on how many processes
 * Aegir can run (AGENTS.md's floors, never ceilings).
 */

#ifndef AEGIR_PROCESS_TABLE_H
#define AEGIR_PROCESS_TABLE_H

#include <aegir/process.h>

#include <stdint.h>

namespace aegir::process {

class ProcessTable {
public:
    /** The table over a caller-owned run of `capacity` rows. */
    ProcessTable(Row* rows, uint32_t capacity) noexcept;

    /** The wire: one call from the registry's client. `caller` is the badge
     *  the kernel reported -- the authority check's subject. The reply words
     *  land in `reply` (up to `capacity`); the answer is how many were
     *  written, zero being the refusal (an unknown method, an argument past the
     *  envelope, or an index past the count). */
    uint32_t handle(uint32_t method, uint64_t const* words, uint32_t word_count,
                    uint64_t caller, uint64_t* reply, uint32_t capacity) noexcept;

    uint32_t count() const noexcept;
    Row const* at(uint32_t index) const noexcept;
    Row const* find(uint64_t pid) const noexcept;

    /** Whether `caller` may break the process whose pid is `target`
     *  (specs/process.md authority): a user breaks a process of its own user
     *  class -- the class the target's own badge carries, so a session service
     *  started by auth but running as the user is the user's to break -- and
     *  the system class, the superuser, breaks any. A user's break of a system
     *  process is therefore refused, which is the rule that a system process
     *  needs elevation. */
    static bool may_break(uint64_t caller, uint64_t target) noexcept;

private:
    bool add(Row const& seed) noexcept;
    bool remove(uint64_t pid) noexcept;
    bool set_flags(uint64_t pid, uint64_t flags, uint64_t caller) noexcept;

    Row* rows_;
    uint32_t capacity_;
    uint32_t count_;
};

}  // namespace aegir::process

#endif  // AEGIR_PROCESS_TABLE_H
