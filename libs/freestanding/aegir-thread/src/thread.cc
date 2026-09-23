/*
 * Starting a thread inside a process -- implementation.
 * See include/aegir/thread.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/bootstrap.h>
#include <aegir/thread.h>
#include <aegir/thread/arch.h>

/* sel4runtime's TLS helpers, declared here rather than by including its
 * header: sel4runtime.h is C-only (specs/userland.md), and the runtime is the
 * only thing that knows the process's TLS layout. */
extern "C" {
uintptr_t sel4runtime_get_tls_size(void);
uintptr_t sel4runtime_write_tls_image(void *tls_memory);
void __sel4runtime_write_tls_variable(uintptr_t thread_pointer, unsigned char *variable,
                                      unsigned char *value, uint64_t size);
}

namespace aegir::thread {

namespace {

constexpr uint64_t kPage = 1ull << seL4_PageBits;

}  // namespace

Builder::Builder(mem::Allocator &allocator, mem::Scratch &scratch, mem::Account &account) noexcept
    : allocator_(allocator), scratch_(scratch), account_(account), problem_("no problem")
{
}

bool Builder::start(Placement const &where, void (*entry)(void *), void *argument,
                    Thread &out) noexcept
{
    problem_ = "no problem";
    if (where.cspace_root == 0 || where.vspace_root == 0 || where.stack_pages == 0) {
        problem_ = "the thread was not told where to run";
        return false;
    }
    uint64_t const stack_bytes = static_cast<uint64_t>(where.stack_pages) * kPage;

    /* The TLS block has to fit the stack it sits on, because that is where it
     * goes: at the top of the stack pages, with the stack pointer below it. */
    uintptr_t const tls_size = sel4runtime_get_tls_size();
    if (tls_size == 0 || tls_size > stack_bytes) {
        problem_ = "the process's TLS image does not fit the thread's stack";
        return false;
    }

    /* The stack, mapped into the process's own address space. `stack_top` is
     * the end of the last page: the thread grows down from there and the TLS
     * block claims the top of this range (below). */
    uintptr_t stack_top = 0;
    for (unsigned page = 0; page < where.stack_pages; ++page) {
        seL4_Error error = seL4_NoError;
        seL4_CPtr const frame =
            allocator_.alloc_object(arch::kPageObject, seL4_PageBits, account_, &error);
        if (frame == 0) {
            problem_ = "no memory for the thread's stack";
            return false;
        }
        void *const at = scratch_.map(frame);
        if (at == nullptr) {
            problem_ = "the thread's stack could not be mapped";
            return false;
        }
        stack_top = reinterpret_cast<uintptr_t>(at) + kPage;
    }
    uintptr_t const stack_lo = stack_top - stack_bytes;

    /* The IPC buffer: a page of its own, mapped where this thread can write it
     * and reach it through its own thread pointer. The frame is part of the
     * TCB's configuration, so it stays this thread's. */
    seL4_Error error = seL4_NoError;
    seL4_CPtr const ipc_frame =
        allocator_.alloc_object(arch::kPageObject, seL4_PageBits, account_, &error);
    if (ipc_frame == 0) {
        problem_ = "no memory for the thread's IPC buffer";
        return false;
    }
    void *const ipc_memory = scratch_.map(ipc_frame);
    if (ipc_memory == nullptr) {
        problem_ = "the thread's IPC buffer could not be mapped";
        return false;
    }

    seL4_CPtr const tcb = allocator_.alloc_object(seL4_TCBObject, seL4_TCBBits, account_, &error);
    if (tcb == 0) {
        problem_ = "no memory for the thread's TCB";
        return false;
    }

    /* The thread shares the process's CNode, so it is configured with that
     * CNode's guard: a service's own-CNode cap is a raw copy whose guard covers
     * the bits the CNode does not index, and a thread with a different guard
     * would resolve the same slot number to a different capability
     * (aegir/bootstrap.h explains). */
    seL4_Word const cspace_guard =
        seL4_CNode_CapData_new(0, seL4_WordBits - bootstrap::kCNodeBits).words[0];
    error = seL4_TCB_Configure(tcb, where.fault_endpoint, where.cspace_root, cspace_guard,
                               where.vspace_root, 0,
                               reinterpret_cast<seL4_Word>(ipc_memory), ipc_frame);
    if (error != seL4_NoError) {
        problem_ = "the thread's TCB could not be configured";
        return false;
    }
    /* A fresh TCB's maximum controlled priority is zero, and SetPriority
     * refuses a priority above the authority's MCP -- the process's own thread
     * is the authority, and its MCP is the priority the process runs at. */
    error = seL4_TCB_SetMCPriority(tcb, seL4_CapInitThreadTCB, where.priority);
    if (error != seL4_NoError) {
        problem_ = "the thread's maximum priority could not be set";
        return false;
    }
    error = seL4_TCB_SetPriority(tcb, seL4_CapInitThreadTCB, where.priority);
    if (error != seL4_NoError) {
        problem_ = "the thread's priority could not be set";
        return false;
    }

    /* The thread's own TLS: the process's image, copied where this thread can
     * reach it, with *its* IPC buffer pointer written into the variable libsel4
     * reads on every syscall. Two threads must not share one, so this is per
     * thread even though the image is the same. */
    auto *tls_memory = reinterpret_cast<void *>(stack_top - tls_size);
    uintptr_t const thread_pointer = sel4runtime_write_tls_image(tls_memory);
    if (thread_pointer == 0) {
        problem_ = "the thread's TLS could not be written";
        return false;
    }
    seL4_IPCBuffer *ipc_pointer = static_cast<seL4_IPCBuffer *>(ipc_memory);
    __sel4runtime_write_tls_variable(thread_pointer,
                                     reinterpret_cast<unsigned char *>(&__sel4_ipc_buffer),
                                     reinterpret_cast<unsigned char *>(&ipc_pointer),
                                     sizeof(ipc_pointer));

    /* Hand the result back before the thread is runnable: resuming it below is
     * enough for it to start inside this call at a timeslice boundary, and an
     * argument that points here must already be valid. */
    out.tcb = tcb;
    out.stack_lo = stack_lo;
    out.stack_top = stack_top;
    out.ipc_buffer = reinterpret_cast<uintptr_t>(ipc_memory);

    seL4_UserContext context = {};
    /* The global pointer the process already uses. A thread started this way
     * skips the crt that computes it, so its first access to a global would
     * fault without this (specs/userland.md); the read is the architecture's,
     * because not every architecture has one (aegir/thread/arch.h). */
    context.gp = arch::global_pointer();
    /* The thread pointer, in the context and not only through a separate TLS
     * invocation: WriteRegisters writes the whole user context, so a zero here
     * would overwrite the base just set and leave the thread with none -- a
     * fault at address zero on its first IPC buffer access. */
    context.tp = thread_pointer;
    context.pc = reinterpret_cast<seL4_Word>(entry);
    /* The entry's one argument, in the register the ABI passes it in. */
    context.a0 = reinterpret_cast<seL4_Word>(argument);
    /* The stack starts below the TLS block, 16-byte aligned as the ABI wants. */
    context.sp = (stack_top - tls_size) & ~static_cast<seL4_Word>(15);
    error = seL4_TCB_WriteRegisters(tcb, 1 /* resume */, 0, sizeof(context) / sizeof(seL4_Word),
                                    &context);
    if (error != seL4_NoError) {
        problem_ = "the thread could not be started";
        return false;
    }
    return true;
}

}  // namespace aegir::thread
