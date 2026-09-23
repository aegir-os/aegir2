/*
 * aegir-cxx-smoke: the hosted C++ runtime's acceptance client.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * It stands the hosted runtime up and exercises it: aegir-heap turns the
 * memory the spawn kit gave this process into musl's mallocng, and the checks
 * in checks.cc run libc++'s containers on top of it. This file is the
 * seL4-facing half and must not include a libc++ header (checks.h explains
 * why); the checks are the libc++ half.
 *
 * What it proves, in order: the untyped, the VSpace root and the window
 * arrived in the bootstrap block; musl's memory syscalls reach our dispatcher
 * (no null __sysinfo); mallocng gets pages and recycles freed ones; libc++'s
 * operator new/delete, std::string, std::vector and std::unordered_map all
 * link and run against them.
 */

#include "checks.h"

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/thread.h>
#include <sel4/sel4.h>
#include <stdlib.h>

namespace {

/* The exit bridge's proof (specs/cxx.md's completion program, step 2): a
 * handler registered at run time lives in musl's list, which only
 * __funcs_on_exit walks, and sel4runtime calls that through the pre-exit hook
 * after main returns. So its marker is the one thing this service prints
 * after main -- the acceptance script's last cue for it. */
void report_atexit()
{
    aegir::debug_write("CXX_ATEXIT_OK\n");
}

/* Static, like the greeter's and the test bed's: the allocator's untyped table
 * and the scratch window's bookkeeping are tens of kilobytes, and a service's
 * stack is pages (specs/userland.md). */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

/* What this process's own objects are charged to; everything here is boot
 * work. */
aegir::mem::Account g_account{"cxx-smoke", 0, 0, 0};

/* The notice between this thread and the one it starts: a notification in this
 * process's CSpace and the worker's report. Globals on purpose -- the worker
 * reaching them is what proves its global pointer. */
struct WorkerReport {
    seL4_CPtr notice;
    volatile uint64_t ran;
    volatile uint64_t allocated;
};
WorkerReport g_worker;

/* What the worker thread runs. A thread has no caller and no startup frame, so
 * it must not return. It reaches a global, allocates through musl (whose
 * syscalls land in the dispatcher), writes to the console, and signals the
 * starter -- each of which depends on a different thing the thread was given:
 * the global pointer, the thread pointer, and the IPC buffer. */
void worker_entry(void *)
{
    void *const block = malloc(4096);
    if (block != nullptr) {
        static_cast<uint8_t *>(block)[0] = 0xa5;
        free(block);
    }
    g_worker.allocated = block != nullptr ? 1 : 0;
    g_worker.ran = 1;
    seL4_Signal(g_worker.notice);
    for (;;) {
        seL4_Word badge = 0;
        seL4_Wait(g_worker.notice, &badge);
    }
}

/* The mapping authority the spawn kit installs: the delegated untyped (page
 * tables and frames are retyped from it), the VSpace root, and the window of
 * free addresses (the give_vspace grant). The pattern is the greeter's and the
 * test bed's. */
bool adopt_memory()
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
        g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::kCNodeBits) - first_free, 0);
        ok = g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                             static_cast<uintptr_t>(window_base),
                             static_cast<uintptr_t>(window_base + window_bytes), &g_objects);
    }
    return ok;
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("\ncxx-smoke: the hosted C++ runtime\n");

    if (!adopt_memory()) {
        aegir::debug_write("  cxx-smoke: FAIL no untyped, vspace or window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    constexpr uint64_t kHeapBytes = 8ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("  cxx-smoke: FAIL the heap could not claim the window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    int failed = aegir::cxx_smoke::run();

    /* A thread in a hosted process: the primitive the test bed proves, on the
     * runtime this process actually uses. It is the floor the runtime's
     * std::thread will stand on (specs/cxx.md step 4): a second seL4 TCB with
     * its own stack, TLS block and IPC buffer, sharing this address space --
     * and running musl's allocator, whose syscalls reach the dispatcher from
     * the new thread too. The starter blocks on the notification first, so on
     * one CPU the worker cannot run until then, and the heap's own state is
     * never touched by two threads at once. */
    {
        uint64_t vspace_slot = 0;
        bool const have_vspace = aegir::bootstrap::capability("vspace", 6, &vspace_slot);
        seL4_Error thread_error = seL4_NoError;
        seL4_CPtr const notice =
            g_objects.alloc_object(seL4_NotificationObject, seL4_NotificationBits,
                                   g_account, &thread_error);
        g_worker.notice = notice;
        g_worker.ran = 0;
        g_worker.allocated = 0;
        aegir::thread::Thread worker{};
        aegir::thread::Builder builder(g_objects, g_scratch, g_account);
        aegir::thread::Placement const where{
            seL4_CapInitThreadCNode,
            static_cast<seL4_CPtr>(vspace_slot),
            seL4_CapNull,
            seL4_MaxPrio - 1,
            4,
        };
        bool const started = notice != 0 && have_vspace &&
                             builder.start(where, worker_entry, nullptr, worker);
        bool ok = started;
        if (started) {
            seL4_Word badge = 0;
            seL4_Recv(notice, &badge);
            ok = g_worker.ran == 1 && g_worker.allocated == 1;
        }
        if (!ok) {
            aegir::debug_write("  cxx-smoke: FAIL a thread could not be started in this "
                               "process\n");
            ++failed;
        } else {
            aegir::debug_write("  cxx-smoke: a second thread ran here, and malloc'd on it\n");
        }
    }

    aegir::debug_write(failed == 0 ? "CXX_SMOKE_OK\n" : "CXX_SMOKE_FAIL\n");

    /* The one run-time exit handler: registered here, run by the bridge once
     * main returns. */
    if (atexit(report_atexit) != 0) {
        aegir::debug_write("  cxx-smoke: FAIL atexit would not register\n");
    }

    /* The boot thread waits for this, so the marker can follow (the same clock
     * the greeter's form-up signal is). Returning, rather than halting,
     * exercises the exit bridge: __funcs_on_exit runs the handler above, then
     * the runtime's exit callback halts the thread. */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    return failed;
}
