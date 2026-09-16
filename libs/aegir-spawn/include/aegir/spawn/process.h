/*
 * Creating a process: Aegir's own spawn path.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This is the work specs/director.md lays out, with the mechanism of each step
 * taken from the pinned tree rather than invented:
 *
 *   1. the binary comes out of the flat initrd, by name;
 *   2. objects are retyped out of the account's untyped memory, so the cost is
 *      charged as it is spent (specs/authority.md);
 *   3. the child's CSpace is built with the Aegir slot layout, and its VSpace
 *      from the ELF's loadable segments (aegir/mem/child_vspace.h);
 *   4. it is given a stack carrying the frame a spawned program expects --
 *      argc/argv/envp/auxv -- where the auxv is what the runtime reads
 *      (projects/sel4runtime/src/env.c:282-320) plus Aegir's own entry pointing
 *      at the bootstrap block;
 *   5. its TCB is configured with the fault endpoint its spawner will hear from,
 *      and started at the ELF's entry point.
 *
 * It is deliberately not libsel4utils: specs/userland.md fixes that position, and
 * what we get for the work is a CSpace layout, a startup ABI and an accounting
 * rule that are Aegir's.
 */

#ifndef AEGIR_SPAWN_PROCESS_H
#define AEGIR_SPAWN_PROCESS_H

#include <aegir/bootstrap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/child_vspace.h>
#include <aegir/mem/vspace.h>
#include <aegir/spawn/elf.h>
#include <aegir/spawn/initrd.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::spawn {

/** A port to install into the child before it starts (specs/services.md): the
 *  endpoint capability, the slot it goes in, and the rights for *this* side. Who
 *  gets which side is the graph's decision, not the spawner's -- the spawner's job
 *  is to install exactly what it is handed and say so in the block. */
struct PortGrant {
    char const *name;
    uint32_t name_length;
    uint64_t slot;
    seL4_CPtr capability;
    seL4_CapRights_t rights;
};

/** What the manifest says about the process to create (specs/services.md). The
 *  strings are views into the manifest text, not copies, and the ports are the
 *  ones this process was granted -- ports it owns and ports it may call. */
struct Request {
    char const *name;
    uint32_t name_length;
    char const *binary;
    uint32_t binary_length;
    char const *account;
    uint32_t account_length;
    uint32_t priority;
    PortGrant const *ports;
    uint32_t port_count;
};

/** A created process, from its creator's side: the capabilities we hold for it. */
struct Process {
    seL4_CPtr cspace;         /* the child's root CNode */
    seL4_CPtr tcb;
    seL4_CPtr fault_endpoint; /* where its faults arrive, in our CSpace */
    seL4_CPtr supervision;    /* the notification it signals when it is ready */
    uint64_t entry;
    uint64_t stack_top;
    uint64_t block;           /* the child's bootstrap block */
};

class Spawner {
public:
    Spawner(mem::Allocator &allocator, mem::Scratch &scratch, mem::Arena &arena,
            Initrd const &initrd) noexcept;

    /** Create, load and start the process a manifest entry describes. */
    bool spawn(Request const &request, mem::Account &account, Process &process) noexcept;

    /** Why the last spawn failed: for the boot report, which is read by people. */
    char const *problem() const noexcept { return problem_; }

private:
    bool fail(char const *what) noexcept;
    bool install(seL4_CPtr into_cspace, uint64_t slot, seL4_CPtr source,
                 seL4_CapRights_t rights) noexcept;
    /** Lay out argc/argv/envp/auxv on the child's stack. Returns the stack
     *  pointer, or 0 when it does not fit. */
    uintptr_t build_start_frame(uint8_t *stack, uint64_t stack_size, uintptr_t stack_top,
                                Elf const &elf, Request const &request, uintptr_t block,
                                uintptr_t ipc_buffer) noexcept;

    mem::Allocator &allocator_;
    mem::Scratch &scratch_;
    mem::Arena &arena_;
    Initrd const &initrd_;
    char const *problem_;
};

}  // namespace aegir::spawn

#endif  // AEGIR_SPAWN_PROCESS_H
