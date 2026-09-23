/*
 * Starting a thread inside a process.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A thread is one seL4 TCB in a process's *own* address space: it shares the
 * CSpace and VSpace with every other thread in the process, and only its
 * stack, its TLS block and its IPC buffer are its own. That is the whole of
 * what this is -- the shape a process's boot thread gets from the spawner, for
 * a thread started by hand (specs/userland.md's threading section records the
 * three things that are easy to get wrong and the symptom of each).
 *
 * It is freestanding on purpose: the TLS image a thread needs is sel4runtime's,
 * which a hosted process does not link. The hosted path -- musl's clone -- will
 * reuse this shape with musl's own TLS, and it belongs with the dispatcher that
 * answers clone (specs/cxx.md step 4).
 */

#ifndef AEGIR_THREAD_H
#define AEGIR_THREAD_H

#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::thread {

/** Where the thread runs, named the way the caller's CSpace does.
 *
 *  A thread is not a process, so it is not *given* a CSpace and a VSpace -- it
 *  uses the ones the process already has. What the kernel needs is a
 *  capability to each, addressable in the caller's CSpace, which is what these
 *  are: the slot of the process's own CNode, and of the VSpace root its
 *  spawner granted. */
struct Placement {
    /** The process's CNode, as the caller names it (seL4_CapInitThreadCNode for
     *  every Aegir process). */
    seL4_CPtr cspace_root;
    /** The process's VSpace root. The root task has it at
     *  seL4_CapInitThreadVSpace; a service was granted it under the name
     *  `vspace` (specs/services.md). */
    seL4_CPtr vspace_root;
    /** Where the thread's faults are delivered, in the caller's CSpace, or
     *  seL4_CapNull for none: a service has no fault endpoint of its own, and
     *  a fault is then not reported anywhere. */
    seL4_CPtr fault_endpoint;
    /** The priority the thread runs at. A fresh TCB has zero, so this must be
     *  set for the thread to be scheduled at all among its peers; it may not
     *  exceed the priority of the process's own thread, which is the authority
     *  the kernel checks against. */
    seL4_Word priority;
    /** How many pages of stack the thread gets. The TLS block sits at the top
     *  of them and the stack grows down below it, so this must be at least one
     *  page and bigger than the process's TLS image. */
    unsigned stack_pages;
};

/** A started thread. Its stack, TLS block and IPC buffer belong to it and stay
 *  mapped in the process for as long as it exists; nothing here takes them
 *  back (a thread that ends is not rebuilt). */
struct Thread {
    seL4_CPtr tcb;
    uintptr_t stack_lo;
    uintptr_t stack_top;
    uintptr_t ipc_buffer;
};

/** A stack and thread pointer the caller has already prepared, for a runtime
 *  (musl) that owns both. `stack` is the initial stack pointer -- 16-byte
 *  aligned, with room below it -- and `thread_pointer` is the value for `tp`;
 *  the caller promises the two do not overlap, which is what musl's
 *  pthread_create arranges. A thread started this way does *not* get
 *  sel4runtime's TLS image (the caller's runtime built its own) -- only its IPC
 *  buffer pointer is written into the TLS. */
struct PreparedStack {
    uintptr_t stack;
    uintptr_t thread_pointer;
};

/** A thread built but not yet running. `prepare` leaves the thread suspended
 *  in everything but name; `resume` is the one register write that starts it.
 *  The split exists for a caller that must hand the thread something only the
 *  builder knows -- the hosted clone handler records the TCB into the child's
 *  start block -- so it can prepare, do that, and then resume. */
struct Pending {
    Thread thread;
    uintptr_t stack_pointer;
    uintptr_t thread_pointer;
    void (*entry)(void *);
    void *argument;
};

class Builder {
public:
    explicit Builder(mem::Allocator &allocator, mem::Scratch &scratch,
                     mem::Account &account) noexcept;

    /**
     * Create and start a thread running `entry(argument)`.
     *
     * `entry` is where the thread's program counter starts and `argument` is
     * its one argument, in the register the ABI passes it in -- a thread gets
     * no startup frame, because it has no loader and no argv. It must not
     * return: there is no caller to return to, and no return address was set.
     *
     * `out` is filled *before* the thread is made runnable, so an argument may
     * safely point at it or at memory it reaches. False on failure, with
     * `problem()` saying what was missing.
     */
    bool start(Placement const &where, void (*entry)(void *), void *argument,
               Thread &out) noexcept;

    /**
     * Build a thread and leave it suspended. `given` is the runtime-owned
     * stack and thread pointer (musl's clone); when it is null the builder
     * allocates the stack and writes sel4runtime's TLS image, the way start()
     * does. `out.thread` is filled before resume(); `out.argument` may be
     * changed between prepare and resume, which is the point of the split.
     */
    bool prepare(Placement const &where, void (*entry)(void *), void *argument,
                 Pending &out, PreparedStack const *given = nullptr) noexcept;

    /** Start a prepared thread. False on failure, with problem() saying why. */
    bool resume(Pending const &pending) noexcept;

    char const *problem() const noexcept { return problem_; }

private:
    mem::Allocator &allocator_;
    mem::Scratch &scratch_;
    mem::Account &account_;
    char const *problem_;
};

}  // namespace aegir::thread

#endif  // AEGIR_THREAD_H
