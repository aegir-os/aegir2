/*
 * Aegir's freeing heap -- implementation. See include/aegir/heap.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The musl side is a patch already in this tree: every one of musl's syscalls
 * routes through the `__sysinfo` function pointer
 * (third_party/patches/projects/musl), so a program that never sets it would
 * call through a null pointer. Aegir's programs never set it today, because
 * nothing in them makes a syscall. A constructor seeds it here, before the
 * standard library's own constructors can allocate, and init() gives the
 * dispatcher somewhere to get memory from.
 *
 * The shapes serve musl's default allocator, mallocng (the vendored full musl
 * is built with no --with-malloc override). It asks for small chunks through
 * SYS_brk -- a monotonic bump whose freed pieces it recycles itself through
 * its groups -- and large ones through SYS_mmap, returning them with
 * SYS_munmap and growing in place through SYS_mremap. SYS_madvise is a hint
 * mallocng may use for reclaim.
 *
 * Everything else is refused with -ENOSYS, which musl reads through
 * __syscall_ret into an errno. The kernel never sees these -- they are
 * answered entirely from the process's own memory.
 *
 * Threads are the second thing this dispatcher answers. musl's pthread_create
 * reaches clone, which this tree routes to __aegir_clone (the musl patch): a
 * new thread is a new seL4 TCB in this process's own address space, started by
 * aegir-thread on the stack and TLS musl prepared. SYS_exit then ends the
 * calling thread rather than the process, clearing the CLONE_CHILD_CLEARTID
 * address the way the kernel would -- musl's locks spin on it, so the clear is
 * what releases a joiner.
 */

#define _GNU_SOURCE 1

/* The C library's <string.h> first, before any seL4 header: seL4's RISC-V
 * syscall header declares strcpy at file scope (seL4_DebugNameThread uses it),
 * and a later `extern "C"` declaration of strcpy -- which is what the C
 * library's has -- conflicts with that C++ one. With the C header first, the
 * plain redeclaration inherits C linkage. memset lives behind it too. */
#include <string.h>

#include <aegir/heap.h>

#include "regions.h"
#include "time.h"

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/console_stream.h>
#include <aegir/console_stream_client.h>
#include <aegir/ipc/port.h>
#include <aegir/launch.h>
#include <aegir/memory.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/network.h>
#include <aegir/posix/files.h>
#include <aegir/posix/spawn.h>
#include <aegir/posix/system.h>
#include <aegir/process.h>
#include <aegir/thread.h>
#include <errno.h>
#include <sched.h>
#include <sel4/sel4.h>
#include <sel4runtime/auxv.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/uio.h>

/* musl's state, which this library seeds:
 *   - `__sysinfo`, the function pointer every syscall lands in
 *     (projects/musl/src/internal/defsysinfo.c defines it; the riscv64 patch
 *     routes all syscalls to it);
 *   - `libc`, whose `page_size` mallocng's alignment reads (the PAGE_SIZE
 *     macro in projects/musl/src/internal/libc.h) and whose `auxv` it walks
 *     for AT_RANDOM (mallocng/glue.h's get_random_secret). `__init_libc`
 *     normally sets both from the vector sel4runtime passes; it is bypassed
 *     here because it would also re-run TLS setup that sel4runtime already
 *     did. The struct layout is the one musl's own internal header declares;
 *     heap.cc names it rather than including that header, which is not on a
 *     client's include path (specs/build.md). */
extern "C" {
extern size_t __sysinfo;
struct aegir_libc {
    char can_do_threads;
    char threaded;
    char secure;
    volatile signed char need_locks;
    int threads_minus_1;
    size_t *auxv;
    struct aegir_tls_module *tls_head;
    size_t tls_size;
    size_t tls_align;
    size_t tls_cnt;
    size_t page_size;
};
extern struct aegir_libc __libc;

/* sel4runtime exposes the vector it was started with; its header is C-only,
 * so the one function needed is declared here (specs/userland.md records the
 * same workaround for the root task). */
auxv_t const *sel4runtime_auxv(void);

/* musl's thread-pointer setup, which __libc_start_main calls and a hosted
 * Aegir process never reaches (see activate_musl_tls below). */
void __init_tls(size_t *aux);
}

namespace aegir::heap {

/* The POSIX surface is a library of its own now -- libs/aegir-posix, one step
 * at the files boundary (specs/posix.md) -- so the dispatcher forwards its
 * file calls across a library edge. The alias keeps this switch's own
 * spelling: every case below names the syscall it answers, not the library it
 * landed in. */
namespace files = aegir::posix::files;

namespace {

/* The heap's state, static like the allocator's (specs/userland.md): a
 * service's stack is pages, and this object is reachable from the syscall
 * dispatcher, which runs wherever musl decided to call into. */
aegir::mem::Allocator *g_allocator = nullptr;
aegir::mem::Scratch *g_scratch = nullptr;

/* The heap's virtual budget, a slice of the granted window. The window's own
 * cursor hands addresses out from the bottom; the heap claims the top. brk
 * grows up from `brk_`; mmap grows down from `mmap_`. They share the region
 * between `base_` and `limit_`. */
uintptr_t base_ = 0;
uintptr_t limit_ = 0;
uintptr_t brk_ = 0;
uintptr_t mmap_ = 0;
bool ready_ = false;

/* The regions an munmap released, awaiting the next mmap. Its nodes live in
 * pages the heap maps below the cursor (regions.h), so growing the list never
 * allocates through the list it is growing, and the region itself is never
 * asked to describe itself. */
aegir::heap::detail::Regions g_free_regions;

bool map_page(uintptr_t address) noexcept;

/* Another node page for the free list: one page reserved from the window's top, the
 * same source as every other run the heap places. Carved off `mmap_` instead it landed
 * *inside the arena*, which maps those same pages -- measured, the kernel's answer was
 * "Virtual address (0x4012d000) already mapped", one page below the window's end
 * (specs/memory.md, "one space, one owner"). */
void *grow_node_region(void *context, unsigned *bytes) noexcept
{
    static_cast<void>(context);
    if (!ready_ || g_scratch == nullptr) {
        return nullptr;
    }
    uintptr_t const page = g_scratch->reserve(1);
    if (page == 0 || !map_page(page)) {
        return nullptr;
    }
    *bytes = static_cast<unsigned>(kPageBytes);
    return reinterpret_cast<void *>(page);
}

/* The record: every run `mmap` has handed out, so that "was this ours?" is answered
 * rather than inferred (specs/memory.md, "The record"). `munmap` removes an entry or
 * refuses. Every memory failure this arc was a release of something the heap did not
 * own -- a run the window could not take, a page past the window's end, the free list's
 * own nodes -- and each was arithmetic on `base_`, `brk_`, `limit_` or alignment
 * standing in for the question.
 *
 * It is a record of what `mmap` *issues*, not a claim about the whole arena: `mprotect`
 * keeps its own bound, because the frame behind an arena page is what the invocation
 * names and the heap's pages are not all mmap runs. Narrowing that check to this record
 * is exactly what broke the C++ smoke's TCB check -- silently, because a refused
 * protection change was not traced (measured: with the record's one extra page and no
 * record, the run is green).
 *
 * The entries live in pages carved below the cursor, the same way the free list's nodes
 * are (grow_node_region), so there is no capacity to choose and none to exceed. */
struct Run {
    uintptr_t base;
    uintptr_t bytes;
};

Run *g_runs = nullptr;
uint32_t g_run_count = 0;
uint32_t g_run_capacity = 0;

bool grow_run_record() noexcept
{
    /* Reserved from the window's top, like the free list's nodes and every other run the
     * heap places: one source, so nothing lands inside the arena (specs/memory.md). */
    if (!ready_ || g_scratch == nullptr) {
        return false;
    }
    uintptr_t const page = g_scratch->reserve(1);
    if (page == 0 || !map_page(page)) {
        return false;
    }
    auto *const more = reinterpret_cast<Run *>(page);
    uint32_t const count = static_cast<uint32_t>(kPageBytes / sizeof(Run));
    for (uint32_t i = 0; i < count; ++i) {
        more[i] = Run{0, 0};
    }
    for (uint32_t i = 0; i < g_run_count; ++i) {
        more[i] = g_runs[i];
    }
    g_runs = more;
    g_run_capacity = count;
    return true;
}

void record_run(uintptr_t base, uintptr_t bytes) noexcept
{
    if (g_run_count == g_run_capacity && !grow_run_record()) {
        return;
    }
    g_runs[g_run_count].base = base;
    g_runs[g_run_count].bytes = bytes;
    ++g_run_count;
}

/* The entry for a run that starts exactly at `base`, or -1. A run is what `mmap`
 * returned, so its start address is the key -- nothing else identifies it. */
int find_run(uintptr_t base) noexcept
{
    for (uint32_t i = 0; i < g_run_count; ++i) {
        if (g_runs[i].base == base) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void forget_run(int index) noexcept
{
    if (index < 0 || static_cast<uint32_t>(index) >= g_run_count) {
        return;
    }
    g_runs[index] = g_runs[--g_run_count];
}

/* The frame behind each mapped page of the arena, so that mprotect can name it
 * again: a mapping's rights change by issuing the Map invocation at the same
 * address (kernel/manual/parts/vspace.tex:294), and that invocation names the
 * frame, which map_page would otherwise drop. 512 frames to a page, one chunk per
 * 512 pages of the arena, and a chunk is mapped the first time a page in its
 * range is mapped -- so the record costs what is *used*. Sizing a table from the
 * whole arena at init was tried and cost the launcher the untyped it spawns
 * commands with (measured: "spawn: FAIL no untyped for the command's runtime"),
 * because a launcher's window is large. */
constexpr uint32_t kFramesPerChunk = kPageBytes / sizeof(seL4_CPtr);

struct FrameChunk {
    FrameChunk *next;
    uint32_t first; /* the arena page index this chunk starts at */
    seL4_CPtr frames[kFramesPerChunk];
};

FrameChunk *g_frame_chunks = nullptr;

/* The chunk holding arena page `index`, made when `create` unless it is already
 * there. Its own page is mapped without being recorded: it is the record, not
 * arena memory a program was given. */
FrameChunk *frame_chunk(uint32_t index, bool create) noexcept
{
    for (FrameChunk *chunk = g_frame_chunks; chunk != nullptr; chunk = chunk->next) {
        if (index >= chunk->first && index - chunk->first < kFramesPerChunk) {
            return chunk;
        }
    }
    if (!create || !ready_ || g_scratch == nullptr) {
        return nullptr;
    }
    /* Reserved from the window's top, like the free list's nodes and the run record:
     * one source for everything the heap places (specs/memory.md, "one space, one
     * owner"). Carved off `mmap_` this landed inside the arena and the kernel answered
     * "Virtual address ... already mapped". */
    uintptr_t const page = g_scratch->reserve(1);
    if (page == 0) {
        return nullptr;
    }
    aegir::mem::Account account{"heap", 0, 0, 0};
    seL4_Error error = seL4_NoError;
    seL4_CPtr const frame =
        g_allocator->alloc_object(seL4_RISCV_4K_Page, seL4_PageBits, account, &error);
    if (frame == 0 || !g_scratch->map_at(page, frame)) {
        return nullptr;
    }
    auto *chunk = reinterpret_cast<FrameChunk *>(page);
    chunk->next = g_frame_chunks;
    chunk->first = index - (index % kFramesPerChunk);
    for (uint32_t i = 0; i < kFramesPerChunk; ++i) {
        chunk->frames[i] = 0;
    }
    g_frame_chunks = chunk;
    return chunk;
}

/* Diagnosis only, off unless the heap is built with -DAEGIR_HEAP_TRACE: every
 * mapping, release and refusal is logged, so the pairs can be replayed offline
 * (scripts/heap_trace.py). The badge tags the lines, because two processes with
 * the same image use the same addresses and a log without it cannot be
 * attributed. It earned its place: it ruled the free list's logic out and left
 * the storage and the reused contents as what to look at (specs/memory.md). */
#ifdef AEGIR_HEAP_TRACE
uint64_t trace_tag() noexcept
{
    static uint64_t tag = 0;
    if (tag == 0) {
        uint64_t badge = 0;
        if (!aegir::bootstrap::badge(&badge) || badge == 0) {
            badge = 1;
        }
        tag = badge;
    }
    return tag;
}

void trace(char const *kind, uintptr_t first, uintptr_t second) noexcept
{
    aegir::debug_write("DISPATCHER_TRACE ");
    aegir::debug_write_hex(trace_tag());
    aegir::debug_write(" ");
    aegir::debug_write(kind);
    aegir::debug_write(" ");
    aegir::debug_write_hex(first);
    aegir::debug_write(" ");
    aegir::debug_write_hex(second);
    aegir::debug_write("\n");
}
#else
void trace(char const *kind, uintptr_t first, uintptr_t second) noexcept
{
    static_cast<void>(kind);
    static_cast<void>(first);
    static_cast<void>(second);
}
#endif

/* Threads (musl's clone): where a new thread runs, what its objects are charged
 * to, and the ids it hands out. A library cannot know a process's VSpace on its
 * own, so init() fills this in from the window it was handed. */
aegir::thread::Placement g_thread_placement{};
aegir::mem::Account g_thread_account{"thread", 0, 0, 0};
int g_next_tid = 0;

/* The calling thread's own TCB, so SYS_exit can end *this* thread instead of
 * the process. It is thread-local on purpose: each thread sets its own before
 * it runs (clone_trampoline), and the process's boot thread leaves it zero --
 * its exit is the process's. */
__thread seL4_CPtr g_self_tcb = 0;

/* And the address that thread must clear when it exits, which is what the
 * kernel's CLONE_CHILD_CLEARTID would do. musl gives clone the thread-list
 * lock here, and both __wait and the joiner's __tl_sync spin until it is
 * cleared -- a real futex wake is not needed, only the clear. */
__thread int *g_clear_tid = nullptr;

uintptr_t align_up(uintptr_t value) noexcept
{
    return (value + kPageBytes - 1) & ~(kPageBytes - 1);
}

/* Map one 4 KiB frame at `address`, from the allocator's untyped, into the
 * adopted window. The heap's pages are charged to its own account -- one
 * accounting hole, charged once and never reclaimed, the same shape as the
 * scratch's throwaway account (aegir/mem/vspace.cc). */
bool map_page(uintptr_t address) noexcept
{
    aegir::mem::Account account{"heap", 0, 0, 0};
    seL4_Error error = seL4_NoError;
    seL4_CPtr const frame = g_allocator->alloc_object(seL4_RISCV_4K_Page,
                                                      seL4_PageBits, account, &error);
    if (frame == 0) {
        return false;
    }
    if (!g_scratch->map_at(address, frame)) {
        return false;
    }
    /* Remembered so that mprotect can name the frame again (the chunk record above).
     * Every page the heap maps inside its window is recorded -- the arena *and* the runs
     * reserved from the window's top, which sit below it. Bounded by the arena instead,
     * a reserved run's pages went unrecorded and mprotect was refused with nothing to
     * see: measured, the memory client's check 10, `mprotect-refused` on one page
     * (specs/memory.md, "one space, one owner"). The registry's own pages now come from
     * `reserve` too, so recording a page outside the arena no longer runs the cycle the
     * note below describes. */
    if (g_scratch != nullptr && address >= g_scratch->base() &&
        address < g_scratch->limit()) {
        /* Indexed from the arena's *top*, which never moves. A floor that can
         * descend is the wrong thing to measure from: an arena at the window's top
         * then lands at a quarter-million-scale index, the record spreads over
         * hundreds of chunks, and every chunk is allocated from the registry --
         * which grows by mapping arena pages, which this record is what remembers.
         * That is a cycle, and it is what hung posix-memory-test
         * (specs/clang-on-aegir.md's Phase 3). */
        uint32_t const index = static_cast<uint32_t>((limit_ - address) / kPageBytes);
        if (FrameChunk *chunk = frame_chunk(index, true)) {
            chunk->frames[index - chunk->first] = frame;
        }
    }
    return true;
}

/* The memory service, found once by name through the bootstrap block, the way
 * the console stream and the clock are (specs/memory.md). A process not given
 * the port has an invalid consumer, and its heap is bounded by its seed as
 * before. */
aegir::ipc::Consumer &memory_port() noexcept
{
    static aegir::ipc::Consumer const service = aegir::ipc::Consumer::find(
        aegir::memory::kPortName, aegir::memory::kPortNameLength);
    return const_cast<aegir::ipc::Consumer &>(service);
}

/* The allocator's untyped source (specs/memory.md): ask mem.main for a chunk
 * and hand the capability back for the allocator to adopt. The chunk is a
 * pristine untyped -- nothing derived from it -- so the heap can retype its
 * next page, page table or mmap from it. The capability rides the reply into
 * the scratch receive slot and is moved into a slot of the allocator's own,
 * because it must stay addressable for every object retyped from it later. */
seL4_CPtr runtime_untyped_source(void *context, seL4_Word *size_bits,
                                 uint64_t *paddr) noexcept
{
    auto *const allocator = static_cast<aegir::mem::Allocator *>(context);
    aegir::ipc::Consumer &service = memory_port();
    if (allocator == nullptr || !service.valid()) {
        return 0;
    }
    uint64_t const request = aegir::memory::kChunkBits;
    uint64_t answer[1] = {};
    bool cap_arrived = false;
    aegir::ipc::WordsReply const reply = service.call_transfer(
        aegir::memory::kMethodAlloc, &request, 1, 0, answer, 1, &cap_arrived);
    if (reply.error != 0 || !cap_arrived) {
        return 0;
    }
    seL4_CPtr const slot = allocator->alloc_slot();
    if (slot == 0 || !aegir::ipc::take_received_cap(slot)) {
        /* The chunk is still in the scratch receive slot; drop it, or the next
         * transfer is refused an occupied slot. */
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                          aegir::bootstrap::kSlotReceiveCap,
                          aegir::bootstrap::endpoint_depth());
        return 0;
    }
    *size_bits = static_cast<seL4_Word>(reply.count >= 1 ? answer[0]
                                                         : aegir::memory::kChunkBits);
    *paddr = 0;
    return slot;
}

/* Give the process musl's TLS. musl's __init_libc rewrites the raw auxv into a
 * flat array indexed by tag before handing it to __init_tls
 * (projects/musl/src/env/__libc_start_main.c:25-29), and a hosted Aegir
 * process never runs __init_libc -- sel4runtime calls main directly
 * (projects/sel4runtime/src/start.c:20). Without this libc.tls_head/tls_size/
 * tls_align stay zero and libc.can_do_threads stays zero, and pthread_create
 * refuses with ENOSYS before it ever reaches clone
 * (projects/musl/src/thread/pthread_create.c:249). Building the same flat
 * array from the vector sel4runtime passes is the one piece of __init_libc the
 * runtime needs.
 *
 * It runs only once the heap can serve the mmap the main thread's TLS is
 * placed with: the image does not fit musl's builtin static TLS, so
 * __init_tls allocates it through our own dispatcher -- which is why this is
 * called from init() and not from a constructor. */
constexpr size_t kAuxCount = 38;  /* musl's AUX_CNT */

void activate_musl_tls() noexcept
{
    size_t aux[kAuxCount] = {};
    auxv_t const *vector = sel4runtime_auxv();
    for (size_t i = 0; vector[i].a_type != AT_NULL; ++i) {
        if (static_cast<size_t>(vector[i].a_type) < kAuxCount) {
            aux[vector[i].a_type] = static_cast<size_t>(vector[i].a_un.a_val);
        }
    }
    /* The IPC buffer pointer lives in TLS and libsel4 reads it on every
     * syscall, so replacing the thread pointer would lose it; carry it into
     * the new TLS. */
    seL4_IPCBuffer *const ipc = seL4_GetIPCBuffer();
    __init_tls(aux);
    __sel4_ipc_buffer = ipc;
}

/* ---- threads: musl's clone ---- */

/* What a new thread reads on its first instruction. musl's pthread_create
 * allocates the stack and the pthread struct (the thread's TLS) and hands both
 * to clone; the function and its argument have nowhere in the Linux ABI to
 * ride, so they are left in the unused part of the child's own IPC buffer page.
 * The TCB is here too, so the child can end itself (SYS_exit). */
struct CloneStart {
    int (*function)(void *);
    void *argument;
    seL4_CPtr tcb;
    int *clear_tid;  /* where a thread's exit clears CLONE_CHILD_CLEARTID */
};

/* Where the CloneStart goes in the child's IPC page: past the seL4_IPCBuffer
 * the kernel itself uses, 16-byte aligned. */
constexpr uintptr_t kCloneStartOffset = (sizeof(seL4_IPCBuffer) + 15) & ~uintptr_t{15};

/* A new thread's first instruction. Its one argument (a0) points at the
 * CloneStart left in its IPC page. It records its own TCB -- so SYS_exit ends
 * it -- and runs the function musl asked for. musl's start never returns: it
 * goes to __pthread_exit, whose last act is a SYS_exit that suspends this
 * thread. */
void clone_trampoline(void *pointer) noexcept
{
    auto *start = static_cast<CloneStart *>(pointer);
    g_self_tcb = start->tcb;
    g_clear_tid = start->clear_tid;
    static_cast<void>(start->function(start->argument));
    seL4_TCB_Suspend(g_self_tcb);
    for (;;) {
    }
}

}  // namespace

/* Before any constructor can allocate: point musl's syscalls at the
 * dispatcher, and give mallocng the two libc fields it reads. The memory
 * syscalls are answered with -ENOSYS until init() claims the window, so an
 * allocation that early fails loudly rather than faulting on a null pointer. */
__attribute__((constructor(200))) void seed_musl(void)
{
    __libc.page_size = kPageBytes;
    __libc.auxv = reinterpret_cast<size_t *>(const_cast<auxv_t *>(sel4runtime_auxv()));
    __sysinfo = reinterpret_cast<size_t>(&vsyscall);
}

bool init(aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch,
          uint64_t bytes) noexcept
{
    if (ready_ || bytes < kPageBytes || bytes > scratch.limit() - scratch.base()) {
        return false;
    }
    g_allocator = &allocator;
    g_scratch = &scratch;

    /* The untyped source (specs/memory.md): when the seed the process was
     * handed runs out, the allocator asks mem.main for another chunk. A
     * process not given the port keeps the seed as its ceiling. */
    allocator.set_untyped_source(runtime_untyped_source, &allocator);

    /* The file layer's capability slots come from the same allocator. */
    files::adopt(allocator, scratch);

    /* The socket layer: find the socket port, if this process was given one,
     * and hand it the runtime's memory -- the allocator and the window a bulk
     * transfer's frame comes from (specs/net.md). */
    network::adopt();
    network::adopt_window(allocator, scratch);

    /* The top of the window, page-aligned so brk arithmetic stays on page
     * boundaries, and below nothing the window already holds. `bytes` is a floor,
     * not a ceiling: the arena takes as much of the window as is there, because a
     * program needing more than the seed should be given what the process has
     * rather than refused (AGENTS.md: no arbitrary or hardcoded limits). What
     * bounded that before -- a frame record sized from the arena at init -- is
     * chunked now, so a large arena costs nothing until it is used. */
    /* The arena is a *reservation* from the window's top -- `Scratch::reserve` walks down
     * from there -- so it sits *beside* the frames a service maps as it starts instead of
     * enclosing them, and every other run the heap places (an mmap, its own bookkeeping)
     * is reserved the same way rather than carved out of a fixed arena. `bytes` is the
     * size of this first run, not a ceiling: `sys_mmap` reserves more when it needs it,
     * which is what lets a 57 MiB linker run where an 8 MiB seed used to stop it
     * (specs/memory.md, "one space, one owner"). */
    if (bytes < kPageBytes) {
        return false;
    }
    uint32_t const wanted = static_cast<uint32_t>(align_up(bytes) / kPageBytes);
    uintptr_t const base = scratch.reserve(wanted);
    if (base == 0) {
        return false;
    }
    base_ = base;
    limit_ = base + static_cast<uintptr_t>(wanted) * kPageBytes;
    brk_ = base_;
    /* `brk` grows within the arena; everything else the heap places is reserved. */
    mmap_ = limit_;
    ready_ = true;
    g_free_regions.set_node_source(grow_node_region, nullptr);
    trace("region", base_, limit_);

    /* Seed again, in case a constructor ordering surprise ran seed_musl()
     * before sel4runtime had the auxv: init() is called from main, long
     * after. */
    __libc.page_size = kPageBytes;
    __libc.auxv = reinterpret_cast<size_t *>(const_cast<auxv_t *>(sel4runtime_auxv()));
    __sysinfo = reinterpret_cast<size_t>(&vsyscall);

    /* And with the heap able to serve it, give the process musl's TLS. */
    activate_musl_tls();

    /* Threads: the placement a clone handler needs comes from the window's own
     * VSpace, which only the process's Scratch knows. The service default
     * priority leaves room below the process's own. */
    g_thread_placement.cspace_root = seL4_CapInitThreadCNode;
    g_thread_placement.vspace_root = scratch.root();
    g_thread_placement.fault_endpoint = seL4_CapNull;
    g_thread_placement.priority = seL4_MaxPrio - 1;
    g_thread_placement.stack_pages = 0;
    return true;
}

/* musl's clone, answered by starting a real seL4 thread in this process's
 * address space. musl has already made the stack and the TLS (its pthread
 * struct); this puts the function and its argument in the child's IPC page and
 * starts it there. It returns the child's id, which is what pthread_create
 * expects on the parent's side -- there is no second return in the child,
 * because the child is a thread that begins at clone_trampoline. */
extern "C" int __aegir_clone(int (*function)(void *), void *stack, int flags, void *argument,
                             int *ptid, void *tls, int *ctid) noexcept
{
    if (!ready_ || g_thread_placement.vspace_root == 0 || function == nullptr ||
        stack == nullptr || tls == nullptr) {
        return -EAGAIN;
    }
    /* Every musl clone is a thread that shares this address space; anything
     * else (a new process, say) is not something this handler starts. */
    if ((flags & (CLONE_VM | CLONE_THREAD)) != (CLONE_VM | CLONE_THREAD)) {
        return -EINVAL;
    }

    aegir::thread::PreparedStack const given{
        reinterpret_cast<uintptr_t>(stack) & ~static_cast<uintptr_t>(15),
        reinterpret_cast<uintptr_t>(tls),
    };
    aegir::thread::Builder builder(*g_allocator, *g_scratch, g_thread_account);
    aegir::thread::Pending pending{};
    if (!builder.prepare(g_thread_placement, clone_trampoline, nullptr, pending, &given)) {
        return -EAGAIN;
    }

    /* Leave the child its instructions in its own IPC page, then point it
     * there -- the hand-off the prepare/resume split exists for. */
    auto *start = reinterpret_cast<CloneStart *>(pending.thread.ipc_buffer + kCloneStartOffset);
    start->function = function;
    start->argument = argument;
    start->tcb = pending.thread.tcb;
    start->clear_tid = (flags & CLONE_CHILD_CLEARTID) != 0 ? ctid : nullptr;
    pending.argument = start;
    if (!builder.resume(pending)) {
        return -EAGAIN;
    }

    int const tid = ++g_next_tid;
    if ((flags & CLONE_PARENT_SETTID) != 0 && ptid != nullptr) {
        *ptid = tid;
    }
    if ((flags & CLONE_CHILD_SETTID) != 0 && ctid != nullptr) {
        *ctid = tid;
    }
    return tid;
}

/* ---- the syscall handlers ---- */

/* SYS_brk: the classic break. Arg 0 returns the current break; otherwise the
 * break moves to a higher address, mapping the pages in between. A break that
 * would pass the mmap side is refused, and a break that would rewind is not
 * asked for -- mallocng only grows it (projects/musl/src/malloc/mallocng/
 * malloc.c's alloc_meta). */
long sys_brk(uintptr_t new_break) noexcept
{
    if (!ready_) {
        return 0;
    }
    if (new_break == 0) {
        return static_cast<long>(brk_);
    }
    if (new_break < brk_ || new_break > mmap_) {
        return static_cast<long>(brk_);
    }
    while (brk_ < new_break) {
        if (!map_page(brk_)) {
            return static_cast<long>(brk_);
        }
        brk_ += kPageBytes;
    }
    return static_cast<long>(brk_);
}

/* SYS_mmap: anonymous private mappings, which is what mallocng asks for, and a
 * file's bytes when the mapping names one -- what LLVM's MemoryBuffer does for
 * every input object file, which specs/clang-on-aegir.md calls the load-bearing
 * gap.
 *
 * The address is ours to choose: grow down from the top of the region, past the
 * brk side. Frames are mapped at the addresses it hands out, and a mapping that
 * names a file has them filled from that file at the given offset, through the
 * POSIX layer's pread -- so a compiler's input arrives by the same path a read()
 * would have taken (specs/posix.md). Past the file's end the pages read as zero,
 * which is what a fresh frame holds and what a reused region is set to. The
 * protection a caller asked for is not enforced here: mprotect's own step is
 * where a mapping's rights change, and narrowing a fresh mapping was measured to
 * cost the cxx smoke a fault on a page it had mapped.
 *
 * MAP_FIXED is refused: the arena's free area is the one interval between the
 * break and the cursor, so a hole punched into it could not be handed back, and
 * replacing a live mapping is not something the heap can do (specs/posix.md).
 *
 * The DISPATCHER_TRACE lines are diagnosis, not a feature: every mapping and
 * release is logged so the pair can be checked offline. They are temporary. */
long sys_mmap(void *addr, size_t length, int prot, int flags, int fd,
              off_t offset) noexcept
{
    static_cast<void>(addr);
    static_cast<void>(prot);
    bool const file_backed = (flags & MAP_ANONYMOUS) == 0;
    if (!ready_ || (flags & MAP_FIXED) != 0 || length == 0 ||
        (file_backed && fd < 0)) {
        return -EINVAL;
    }
    uintptr_t const needed = align_up(length);
    uintptr_t base = 0;
    if (void *released = g_free_regions.take(needed)) {
        /* mmap promises zero-filled pages, and a released region is not: the
         * bytes it holds are the last owner's (specs/memory.md). */
        memset(released, 0, needed);
        base = reinterpret_cast<uintptr_t>(released);
        trace("mmap-reused", base, needed);
    } else {
        /* A run reserved from the window's top (`Scratch::reserve` walks down from
         * there), not carved from a fixed arena: the window is what the process has, and
         * its reservation cursor is the only cap. A run handed back is reserved again, or
         * reused from `g_free_regions` above -- this is the bound that said "Out of
         * memory" when a 97 MiB compiler tried to link (specs/memory.md). */
        uintptr_t const reserved =
            g_scratch->reserve(static_cast<unsigned>(needed / kPageBytes));
        if (reserved == 0) {
            trace("mmap-refused", length, 0);
            return -ENOMEM;
        }
        base = reserved;
        for (uintptr_t page = 0; page < needed / kPageBytes; ++page) {
            if (!map_page(base + page * kPageBytes)) {
                trace("mmap-refused", length, 0);
                return -ENOMEM;
            }
        }
    }
    if (file_backed) {
        long const filled =
            files::pread(fd, reinterpret_cast<void *>(base), needed, offset);
        if (filled < 0) {
            trace("mmap-file-refused", length, 0);
            return filled;
        }
        trace("mmap-file", base, static_cast<uintptr_t>(filled));
    } else {
        trace("mmap", base, needed);
    }
    /* The run is ours from here on, whether it was carved fresh or handed back by the
     * free list: recorded, so `munmap` asks the record instead of reasoning from bounds
     * (specs/memory.md, "The record"). */
    record_run(base, needed);
    return static_cast<long>(base);
}

/* SYS_munmap: the pages stay mapped -- the address space is committed to the
 * heap, one carve we do not return -- so an mmap that follows hands back over
 * the same memory. The recycling that matters is musl's own: mallocng's
 * groups reuse the small pieces, and the large individually-mapped ones are
 * rare enough in a GUI that the virtual budget holds. Returning the frames to
 * the kernel is a later refinement, not a correctness need.
 *
 * The DISPATCHER_TRACE line is temporary diagnosis (see sys_mmap). */
long sys_munmap(void *addr, size_t length) noexcept
{
    trace("munmap", reinterpret_cast<uintptr_t>(addr), length);
    if (!ready_ || addr == nullptr || length == 0) {
        return -EINVAL;
    }
    uintptr_t const base = reinterpret_cast<uintptr_t>(addr);
    uintptr_t const bytes = align_up(length);
    /* The record answers "was this ours?". The bounds this used to reason from --
     * `base_`, `brk_`, `limit_`, alignment -- each admitted a range the heap did not
     * own, and one of them released a run whose last page sat at `limit_ + 0xf` for
     * the next caller (specs/memory.md, "The record"). A run is released only when
     * `mmap` handed it out, whole. */
    int const at = find_run(base);
    if (at < 0 || g_runs[at].bytes != bytes) {
        trace("munmap-refused", base, bytes);
        return -EINVAL;
    }
    forget_run(at);
    if (!g_free_regions.give(base, bytes)) {
        trace("munmap-refused", base, bytes);
        return -EINVAL;
    }
    /* And back to the window: `Scratch::release` takes the run when it ends at the
     * reservation cursor and does nothing otherwise -- the space is in the heap's list
     * either way, so a run that is not adjacent is handed out again rather than lost
     * (specs/memory.md, "one space, one owner"). */
    g_scratch->release(base, static_cast<unsigned>(bytes / kPageBytes));
    return 0;
}

/* SYS_mremap: growing in place is not possible -- the next page is not
 * necessarily free -- so the answer is "no", and mallocng's realloc falls
 * back to the fresh-malloc + copy + free path, which our mmap and munmap
 * shapes serve. */
long sys_mremap(void *old_address, size_t old_size, size_t new_size,
                int flags, ...) noexcept
{
    static_cast<void>(old_address);
    static_cast<void>(old_size);
    static_cast<void>(new_size);
    static_cast<void>(flags);
    return -ENOMEM;
}

/* POSIX protection bits as the kernel's rights, so that a read-only mapping is
 * read-only to the hardware and not merely in the caller's intentions: read is
 * always allowed -- a mapping nothing may read is not something a program asks
 * for -- and write follows PROT_WRITE (specs/posix.md). */
seL4_CapRights_t rights_for(int prot) noexcept
{
    return seL4_CapRights_new(0, 0, 1, (prot & PROT_WRITE) != 0 ? 1 : 0);
}

/* Give the pages of `[address, address + length)`, already mapped, the rights
 * `rights` carries. The manual's own words are that a mapping's attributes can be
 * updated on an existing mapping with a Map invocation at the same address
 * (kernel/manual/parts/vspace.tex:294), and this is that invocation. With no
 * unmap first: an unmap whose remap does not succeed leaves the process without
 * memory it owns, and that is worse than a protection that did not change. */
bool set_rights(uintptr_t address, size_t length, seL4_CapRights_t rights) noexcept
{
    for (uintptr_t at = address; at < address + length; at += kPageBytes) {
        /* The *window*, not the arena: everything the heap places -- an mmap's run, its
         * own bookkeeping, the arena itself -- lies inside the window the process was
         * given, and only the arena lies between base_ and limit_. Bounded by the arena,
         * this refused mprotect on any mmap'd page, and silently: measured, the memory
         * client's check 10 ("mprotect a page read-only") came back ENOMEM with the
         * kernel saying nothing (specs/memory.md, "one space, one owner"). */
        if (!ready_ || g_scratch == nullptr || at < g_scratch->base() ||
            at >= g_scratch->limit()) {
            return false;
        }
        uint32_t const index = static_cast<uint32_t>((limit_ - at) / kPageBytes);
        FrameChunk *chunk = frame_chunk(index, false);
        if (chunk == nullptr || chunk->frames[index - chunk->first] == 0) {
            return false;
        }
        if (!g_scratch->map_at(at, chunk->frames[index - chunk->first], rights)) {
            return false;
        }
    }
    return true;
}

/* SYS_mprotect: a mapping's protection, changed in place (specs/posix.md). The
 * whole range must be one the arena mapped, because the frame behind each page is
 * what the invocation names; a page it never mapped is ENOMEM, as a kernel would
 * say for a range it cannot back.
 *
 * Deliberately not the record: the ledger says what `mmap` *issued*, and the heap's
 * pages are not all mmap runs -- narrowing this check to it broke the C++ smoke's TCB
 * check silently. Both refusals are traced, because a resolution that is never written
 * down is how that took a dozen runs to find (specs/memory.md, "The record"). */
long sys_mprotect(void *addr, size_t length, int prot) noexcept
{
    if (!ready_ || addr == nullptr || length == 0 ||
        (reinterpret_cast<uintptr_t>(addr) & (kPageBytes - 1)) != 0) {
        return -EINVAL;
    }
    uintptr_t const address = reinterpret_cast<uintptr_t>(addr);
    uintptr_t const needed = align_up(length);
    /* The window, not the arena: `mmap`'s runs and the heap's own bookkeeping are
     * reserved from the window's top, so they sit *below* `base_` -- and an arena bound
     * here refused a legitimate protection change with no kernel error to see
     * (specs/memory.md, "one space, one owner"). */
    if (g_scratch == nullptr || address < g_scratch->base() ||
        address + needed > g_scratch->limit()) {
        trace("mprotect-refused", address, needed);
        return -ENOMEM;
    }
    if (!set_rights(address, needed, rights_for(prot))) {
        trace("mprotect-refused", address, needed);
        return -ENOMEM;
    }
    trace("mprotect", address, needed);
    return 0;
}

/* SYS_madvise: a hint. mallocng asks for reclaim; the pages are already ours
 * and mapped, so there is nothing to advise. */
long sys_madvise(void *addr, size_t length, int advice) noexcept
{
    static_cast<void>(addr);
    static_cast<void>(length);
    static_cast<void>(advice);
    return 0;
}

/* The session's console stream, when this process was given one: the terminal
 * hands a command a caller copy of its stream (specs/shell.md), and fd 0/1/2
 * route there instead of the debug serial. A process without one -- every
 * boot service -- keeps the serial. Found once, on first use. */
aegir::ipc::Consumer &console_stream() noexcept
{
    static aegir::ipc::Consumer const stream = aegir::ipc::Consumer::find(
        aegir::console::kStreamPortName, aegir::console::kStreamPortNameLength);
    return const_cast<aegir::ipc::Consumer &>(stream);
}

/* The break source the spawner bound to this TCB (specs/process.md): a
 * notification it also handed the registry, which signals it to abort us. The
 * spawner names it `break.source` in the block; a process the spawner gave none
 * -- a boot service -- has no source and polls nothing. Found once, on first
 * use. */
seL4_CPtr break_source() noexcept
{
    static seL4_CPtr const source = [] {
        uint64_t slot = 0;
        if (aegir::bootstrap::capability("break.source", 12, &slot)) {
            return static_cast<seL4_CPtr>(slot);
        }
        return static_cast<seL4_CPtr>(0);
    }();
    return source;
}

void report_exit(int status) noexcept;

/* The attention flags a break has signalled since the program last took them
 * (specs/process.md). The signal's badge lands in `sender` and *is* the flags --
 * the registry mints the source with them -- so a poll reads them and clears the
 * notification; they are latched here until the program takes them, so a poll
 * the runtime makes does not swallow a flag only the program can act on. */
static uint64_t g_break_flags = 0;

/* Poll the process's own break source, latching any flags it carries. One
 * non-blocking syscall (seL4_NBRecv); a process with no source polls nothing.
 * This is the idle wait's check (specs/process.md Phase 2): the runtime makes it
 * before it blocks on input, and a program that acts on a flag makes it between
 * its own operations. */
static void refresh_break() noexcept
{
    seL4_CPtr const source = break_source();
    if (source == 0) {
        return;
    }
    seL4_Word badge = 0;
    (void)seL4_Poll(source, &badge);
    g_break_flags |= badge;
}

uint64_t take_break_flags() noexcept
{
    refresh_break();
    uint64_t const flags = g_break_flags;
    g_break_flags = 0;
    return flags;
}

/* A redirected standard stream (specs/shell.md): the spawner put a path in the
 * bootstrap block, and the first use opens it. -1 is "no redirection" and
 * leaves the stream the console's; a failed open returns its errno. The path is
 * length-prefixed in the block, not NUL-terminated, so it is copied out. */
long redirected_fd(aegir::bootstrap::EntryKind kind) noexcept
{
    uint32_t length = 0;
    char const *given = aegir::bootstrap::string(kind, &length);
    if (given == nullptr || length == 0 || length > aegir::console::kStreamBytesMax) {
        return -1;
    }
    char path[aegir::console::kStreamBytesMax + 1];
    for (uint32_t i = 0; i < length; ++i) {
        path[i] = given[i];
    }
    path[length] = '\0';
    return kind == aegir::bootstrap::EntryKind::StdIn ? files::open_for_read(path)
                                                      : files::open_for_write(path);
}

long stdin_fd() noexcept
{
    static long const fd = redirected_fd(aegir::bootstrap::EntryKind::StdIn);
    return fd;
}

long stdout_fd() noexcept
{
    static long const fd = redirected_fd(aegir::bootstrap::EntryKind::StdOut);
    return fd;
}

/* SYS_write: a standard stream goes to the console stream when the process has
 * one, the debug serial otherwise; any other fd is a file the filesystem arc
 * opened. musl's stdio and the standard library's diagnostics both reach here
 * (specs/cxx.md step 5, specs/shell.md's Phase 4). */
long sys_write(int fd, void const *buffer, size_t length) noexcept
{
    if (network::owns(fd)) {
        return network::sendto(fd, buffer, length, 0, nullptr, 0);
    }
    if (fd == 1) {
        /* Redirected output (specs/shell.md): the spawner named a file, and the
         * write lands there rather than on the grid. */
        long const redirected = stdout_fd();
        if (redirected != -1) {
            return redirected < 0 ? redirected : files::write(redirected, buffer, length);
        }
    }
    if (fd == 1 || fd == 2) {
        aegir::ipc::Consumer &stream = console_stream();
        if (stream.valid()) {
            auto const *bytes = static_cast<char const *>(buffer);
            uint64_t total = 0;
            while (total < length) {
                uint32_t const chunk = static_cast<uint32_t>(
                    length - total < aegir::console::kStreamBytesMax
                        ? length - total
                        : aegir::console::kStreamBytesMax);
                uint32_t const wrote =
                    aegir::console::stream_write(stream, bytes + total, chunk);
                if (wrote == 0) {
                    break;
                }
                total += wrote;
            }
            return static_cast<long>(total);
        }
        auto const *bytes = static_cast<uint8_t const *>(buffer);
        for (size_t i = 0; i < length; ++i) {
            seL4_DebugPutChar(static_cast<char>(bytes[i]));
        }
        return static_cast<long>(length);
    }
    return files::write(fd, buffer, length);
}

/* SYS_read: fd 0 is the console stream. The read waits inside its call --
 * the terminal holds its reply until a key arrives or the command ends, where
 * an empty answer is end of input (specs/signal.md). A process with no stream
 * has no stdin. Any other fd is a file. */
long sys_read(int fd, void *buffer, size_t length) noexcept
{
    /* The idle wait's check (specs/process.md): a **C** aborts the process where
     * it is about to block on input. D, E and F are latched for the program to
     * act on (take_break_flags) -- the shell halts its frame on D. */
    refresh_break();
    if ((g_break_flags & aegir::process::kAttnC) != 0) {
        report_exit(static_cast<int>(aegir::console::kBreakStatus));
    }
    if (network::owns(fd)) {
        return network::recvfrom(fd, buffer, length, 0, nullptr, nullptr);
    }
    if (fd == 0) {
        /* Redirected input (specs/shell.md): the spawner named a file, and the
         * read drains it to its end rather than the console's queue. A NIL:
         * input is at its end at once, so `cmd <NIL:` reads EOF. */
        long const redirected = stdin_fd();
        if (redirected != -1) {
            return redirected < 0 ? redirected : files::read(redirected, buffer, length);
        }
        aegir::ipc::Consumer &stream = console_stream();
        if (!stream.valid()) {
            return -EBADF;
        }
        uint32_t const want = length < aegir::console::kStreamBytesMax
                                  ? static_cast<uint32_t>(length)
                                  : aegir::console::kStreamBytesMax;
        /* The read waits inside the call: the terminal holds the reply until a
         * key arrives or the command ends, so there is no doorbell to park on
         * and nothing to poll (specs/signal.md). An empty answer is end of
         * input. */
        return static_cast<long>(
            aegir::console::stream_read(stream, static_cast<char *>(buffer), want));
    }
    return files::read(fd, buffer, length);
}

/* Wait for a child the launcher started (specs/posix.md): call the launcher's
 * `wait`, which holds the reply until the child ends, and encode the status as
 * Linux wait4 does -- the exit code in bits 8..15, the low byte zero so
 * WIFEXITED is true. */
long wait_child(int pid, int *status) noexcept
{
    aegir::ipc::Consumer const launcher = aegir::ipc::Consumer::find(
        aegir::launch::kPortName, aegir::launch::kPortNameLength);
    uint64_t badge = 0;
    if (!launcher.valid() || pid <= 0 || !aegir::posix::take_child_badge(pid, &badge)) {
        return -ECHILD;
    }
    uint64_t answer[2] = {};
    aegir::ipc::WordsReply const reply =
        launcher.call_words(aegir::launch::kMethodWait, &badge, 1, answer, 2);
    if (reply.error != 0 || reply.count < 2 || answer[0] != 1) {
        return -ECHILD;
    }
    if (status != nullptr) {
        *status = static_cast<int>((answer[1] & 0xffu) << 8);
    }
    return pid;
}

/* Report this process's end to its spawner (specs/launch.md's "Waiting for a
 * child"): the launcher attributes the message to our own pid by the kernel's
 * badge and files it, so a parent's `wait` can answer. A process the launcher
 * gave no caller half (a boot service) has nothing to reach; best-effort.
 *
 * One-way, not a call: there is nothing to wait for, and the process has an
 * exit still to make -- the stream exit that reaps it. Waiting for a reply
 * here would put the launcher's whole serve loop on the critical path of every
 * command's exit; the message is received, and the sender released, before the
 * launcher files it. */
void report_to_launcher(int status) noexcept
{
    aegir::ipc::Consumer const launcher = aegir::ipc::Consumer::find(
        aegir::launch::kPortName, aegir::launch::kPortNameLength);
    if (!launcher.valid()) {
        return;
    }
    uint64_t const word = static_cast<uint64_t>(status);
    launcher.send_words(aegir::launch::kMethodExited, &word, 1);
}

/* The process is ending: report the status through the console stream, so the
 * shell's return-code line has its number (the interim until a status travels
 * another way), tell the spawner's `wait`, then halt. The command's own badge
 * rides with the stream message, so the terminal can tell a foreground
 * command's exit from a background `Run`'s (specs/shell.md). A process with no
 * stream still reports to its spawner and halts. */
void report_exit(int status) noexcept
{
    /* Tell the spawner's `wait` first (specs/launch.md's "Waiting for a
     * child"): the terminal's stream exit below triggers a `release` that reaps
     * this process, so the launcher must have the end before that happens, or a
     * parent waiting on it is never answered. */
    report_to_launcher(status);
    aegir::ipc::Consumer &stream = console_stream();
    if (stream.valid()) {
        uint64_t badge = 0;
        (void)aegir::bootstrap::badge(&badge);
        (void)aegir::console::stream_exit(stream, static_cast<uint64_t>(status), badge);
    }
    aegir::halt();
}

/* A hosted process ends through musl, not through sel4runtime's exit bridge.
 * `std::exit` reaches exit_group and report_exit, but a program that merely
 * returns from main leaves sel4runtime's `__sel4runtime_start_main` to call
 * its exit callback -- which aegir-runtime installed as a bare halt. Replace
 * that callback with report_exit, so `return N` from main reports N through
 * the console stream exactly as `std::exit(N)` does, and a hosted program
 * never has to name the exit call at all. The constructor priority is above
 * aegir-runtime's bridge (201), so this is the one that survives. */
extern "C" {
typedef void sel4runtime_exit_cb(int code);
sel4runtime_exit_cb *sel4runtime_set_exit(sel4runtime_exit_cb *cb);
}

namespace {

void hosted_exit(int code) noexcept
{
    /* A program that merely returned from main left its stdio buffers full:
     * musl's own exit flushes them, but sel4runtime's path calls this
     * callback instead (the comment above). Flush here, so a command's last
     * output is not lost -- the bytes a pipeline's writer still held, the
     * tail of a redirected file (specs/shell.md, specs/pipe.md). A second
     * flush on the exit() path is a no-op. */
    (void)fflush(nullptr);
    report_exit(code);
}

}  // namespace

__attribute__((constructor(202))) void install_hosted_exit() noexcept
{
    sel4runtime_set_exit(hosted_exit);
}

/* SYS_writev: what musl's stdio actually uses. Without it, vfprintf's output
 * (libc++'s verbose-abort message among it) is lost and the abort looks
 * silent. A standard stream goes to the debug console; any other fd is a file
 * the filesystem arc opened (specs/cxx.md step 5). */
long sys_writev(int fd, void const *iov, int count) noexcept
{
    auto const *vectors = static_cast<struct iovec const *>(iov);
    long total = 0;
    for (int i = 0; i < count; ++i) {
        long const wrote = sys_write(fd, vectors[i].iov_base, vectors[i].iov_len);
        if (wrote <= 0) {
            return total > 0 ? total : wrote;
        }
        total += wrote;
        if (static_cast<size_t>(wrote) < vectors[i].iov_len) {
            return total;
        }
    }
    return total;
}

/* SYS_readv: what musl's *buffered* input uses -- `fread`, and so every
 * `std::fread` the toolkit's file code and the shell's Type and command
 * loading make. Without it, the runtime's `read` is right and the stdio on
 * top of it returns zero bytes, which reads as an empty file rather than as
 * a missing syscall. One vector is the FILE's own buffer and the next the
 * caller's; both are filled from the same fd, in order. */
long sys_readv(int fd, void const *iov, int count) noexcept
{
    auto const *vectors = static_cast<struct iovec const *>(iov);
    if (fd == 0) {
        /* Standard input: the console stream's poll, the same as `read`.
         * musl's buffered input reaches here, so a `fread` on stdin is the
         * stream's bytes and not a file's (specs/cxx.md, specs/shell.md). */
        long total = 0;
        for (int i = 0; i < count; ++i) {
            long const got = sys_read(0, vectors[i].iov_base, vectors[i].iov_len);
            if (got < 0) {
                return total > 0 ? total : got;
            }
            total += got;
            if (static_cast<size_t>(got) < vectors[i].iov_len) {
                break;
            }
        }
        return total;
    }
    long total = 0;
    for (int i = 0; i < count; ++i) {
        long const got = files::read(fd, vectors[i].iov_base, vectors[i].iov_len);
        if (got < 0) {
            return total > 0 ? total : got;
        }
        total += got;
        if (static_cast<size_t>(got) < vectors[i].iov_len) {
            break;
        }
    }
    return total;
}

/* The dispatcher: musl's syscall table, one switch. The va_arg reads are the
 * Linux ABI's argument order for each call (projects/musl/arch/riscv64/bits/
 * syscall.h.in has the numbers). */
long vsyscall(long sysnum, ...) noexcept
{
    va_list ap;
    va_start(ap, sysnum);
    long ret = -ENOSYS;
    switch (sysnum) {
    case 214: /* SYS_brk */
        ret = sys_brk(static_cast<uintptr_t>(va_arg(ap, unsigned long)));
        break;
    case 222: /* SYS_mmap */
        ret = sys_mmap(va_arg(ap, void *), va_arg(ap, size_t),
                       va_arg(ap, int), va_arg(ap, int), va_arg(ap, int),
                       va_arg(ap, off_t));
        break;
    case 226: /* SYS_mprotect */
        ret = sys_mprotect(va_arg(ap, void *), va_arg(ap, size_t), va_arg(ap, int));
        break;
    case 215: /* SYS_munmap */
        ret = sys_munmap(va_arg(ap, void *), va_arg(ap, size_t));
        break;
    case 216: /* SYS_mremap */
        ret = sys_mremap(va_arg(ap, void *), va_arg(ap, size_t),
                         va_arg(ap, size_t), va_arg(ap, int));
        break;
    case 233: /* SYS_madvise */
        ret = sys_madvise(va_arg(ap, void *), va_arg(ap, size_t),
                          va_arg(ap, int));
        break;
    case 64: /* SYS_write */
        ret = sys_write(va_arg(ap, int), va_arg(ap, void const *),
                        va_arg(ap, size_t));
        break;
    case 66: /* SYS_writev */
        ret = sys_writev(va_arg(ap, int), va_arg(ap, void const *),
                         va_arg(ap, int));
        break;
    case 65: /* SYS_readv */
        ret = sys_readv(va_arg(ap, int), va_arg(ap, void const *),
                        va_arg(ap, int));
        break;
    /* ---- the file calls (specs/cxx.md step 5): answered from aegir::vfs,
     * one function per syscall. The numbers are musl's riscv64 table
     * (arch/riscv64/bits/syscall.h.in), the same source the memory cases
     * above are read from. ---- */
    case 17: /* SYS_getcwd */
        ret = files::getcwd(va_arg(ap, char *), va_arg(ap, size_t));
        break;
    case 25: /* SYS_fcntl */
        ret = files::fcntl(va_arg(ap, int), va_arg(ap, int), va_arg(ap, long));
        break;
    case 34: /* SYS_mkdirat */
        ret = files::mkdirat(va_arg(ap, int), va_arg(ap, char const *),
                             va_arg(ap, int));
        break;
    case 35: /* SYS_unlinkat */
        ret = files::unlinkat(va_arg(ap, int), va_arg(ap, char const *),
                              va_arg(ap, int));
        break;
    case 276: /* SYS_renameat2: musl's rename() on riscv64 */
        ret = files::renameat2(va_arg(ap, int), va_arg(ap, char const *),
                               va_arg(ap, int), va_arg(ap, char const *),
                               va_arg(ap, unsigned));
        break;
    case 45: /* SYS_truncate: musl's std::filesystem::resize_file */
        ret = files::truncate(va_arg(ap, char const *), va_arg(ap, long));
        break;
    case 46: /* SYS_ftruncate */
        ret = files::ftruncate(va_arg(ap, int), va_arg(ap, long));
        break;
    case 5: /* SYS_setxattr */
        ret = files::setxattr(va_arg(ap, char const *), va_arg(ap, char const *),
                              va_arg(ap, void const *), va_arg(ap, size_t),
                              va_arg(ap, int));
        break;
    case 6: /* SYS_lsetxattr */
        ret = files::lsetxattr(va_arg(ap, char const *), va_arg(ap, char const *),
                               va_arg(ap, void const *), va_arg(ap, size_t),
                               va_arg(ap, int));
        break;
    case 7: /* SYS_fsetxattr */
        ret = files::fsetxattr(va_arg(ap, int), va_arg(ap, char const *),
                               va_arg(ap, void const *), va_arg(ap, size_t),
                               va_arg(ap, int));
        break;
    case 8: /* SYS_getxattr */
        ret = files::getxattr(va_arg(ap, char const *), va_arg(ap, char const *),
                              va_arg(ap, void *), va_arg(ap, size_t));
        break;
    case 9: /* SYS_lgetxattr */
        ret = files::lgetxattr(va_arg(ap, char const *), va_arg(ap, char const *),
                               va_arg(ap, void *), va_arg(ap, size_t));
        break;
    case 10: /* SYS_fgetxattr */
        ret = files::fgetxattr(va_arg(ap, int), va_arg(ap, char const *),
                               va_arg(ap, void *), va_arg(ap, size_t));
        break;
    case 11: /* SYS_listxattr */
        ret = files::listxattr(va_arg(ap, char const *), va_arg(ap, char *),
                               va_arg(ap, size_t));
        break;
    case 12: /* SYS_llistxattr */
        ret = files::llistxattr(va_arg(ap, char const *), va_arg(ap, char *),
                                va_arg(ap, size_t));
        break;
    case 13: /* SYS_flistxattr */
        ret = files::flistxattr(va_arg(ap, int), va_arg(ap, char *),
                                va_arg(ap, size_t));
        break;
    case 14: /* SYS_removexattr */
        ret = files::removexattr(va_arg(ap, char const *), va_arg(ap, char const *));
        break;
    case 15: /* SYS_lremovexattr */
        ret = files::lremovexattr(va_arg(ap, char const *),
                                  va_arg(ap, char const *));
        break;
    case 16: /* SYS_fremovexattr */
        ret = files::fremovexattr(va_arg(ap, int), va_arg(ap, char const *));
        break;
    case 160: /* SYS_uname: what this system says it is (aegir/release.h) */
        ret = aegir::posix::system::uname(va_arg(ap, void *));
        break;
    case 113: /* SYS_clock_gettime: the clock and timer services behind it */
        ret = time::clock_gettime(va_arg(ap, int), va_arg(ap, void *));
        break;
    case 101: /* SYS_nanosleep: the timer service behind it (aegir/timer.h) */
        ret = time::nanosleep(va_arg(ap, void const *), va_arg(ap, void *));
        break;
    case 48: /* SYS_faccessat: `access` is this call with AT_FDCWD, and a linker asks
              * it whether a file is there before it opens it (files.h's faccessat,
              * specs/clang-on-aegir.md -- this is what answered "cannot find linker
              * script" and "unable to find library" while the files were staged). */
        ret = files::faccessat(va_arg(ap, int), va_arg(ap, char const *),
                               va_arg(ap, int), va_arg(ap, int));
        break;
    case 49: /* SYS_chdir */
        ret = files::chdir(va_arg(ap, char const *));
        break;
    case 52: /* SYS_fchmod: musl's fchmod, and the fd form of Protect */
        ret = files::fchmod(va_arg(ap, int), va_arg(ap, int));
        break;
    case 53: /* SYS_fchmodat: musl's chmod on riscv64 (three args; fchmodat2 is 452) */
        ret = files::fchmodat(va_arg(ap, int), va_arg(ap, char const *),
                              va_arg(ap, int));
        break;
    case 56: /* SYS_openat */
        ret = files::openat(va_arg(ap, int), va_arg(ap, char const *),
                            va_arg(ap, int), va_arg(ap, int));
        break;
    case 57: { /* SYS_close: a socket fd is the socket layer's, a file fd the files' */
        int const fd = va_arg(ap, int);
        ret = network::owns(fd) ? network::close(fd) : files::close(fd);
        break;
    }
    case 198: /* SYS_socket */
        ret = network::socket(va_arg(ap, int), va_arg(ap, int), va_arg(ap, int));
        break;
    case 200: /* SYS_bind */
        ret = network::bind(va_arg(ap, int), va_arg(ap, void const *), va_arg(ap, int));
        break;
    case 201: /* SYS_listen */
        ret = network::listen(va_arg(ap, int), va_arg(ap, int));
        break;
    case 202: /* SYS_accept */
        ret = network::accept(va_arg(ap, int), va_arg(ap, void *), va_arg(ap, int *));
        break;
    case 242: /* SYS_accept4: the flags are accepted; the stack has none to set */
        ret = network::accept(va_arg(ap, int), va_arg(ap, void *), va_arg(ap, int *));
        break;
    case 203: /* SYS_connect */
        ret = network::connect(va_arg(ap, int), va_arg(ap, void const *), va_arg(ap, int));
        break;
    case 204: /* SYS_getsockname */
        ret = network::getsockname(va_arg(ap, int), va_arg(ap, void *), va_arg(ap, int *));
        break;
    case 205: /* SYS_getpeername */
        ret = network::getpeername(va_arg(ap, int), va_arg(ap, void *), va_arg(ap, int *));
        break;
    case 206: /* SYS_sendto: musl's send and sendto both land here */
        ret = network::sendto(va_arg(ap, int), va_arg(ap, void const *), va_arg(ap, size_t),
                              va_arg(ap, int), va_arg(ap, void const *), va_arg(ap, int));
        break;
    case 207: /* SYS_recvfrom: musl's recv and recvfrom both land here */
        ret = network::recvfrom(va_arg(ap, int), va_arg(ap, void *), va_arg(ap, size_t),
                                va_arg(ap, int), va_arg(ap, void *), va_arg(ap, int *));
        break;
    case 208: /* SYS_setsockopt */
        ret = network::setsockopt(va_arg(ap, int), va_arg(ap, int), va_arg(ap, int),
                                  va_arg(ap, void const *), va_arg(ap, int));
        break;
    case 209: /* SYS_getsockopt */
        ret = network::getsockopt(va_arg(ap, int), va_arg(ap, int), va_arg(ap, int),
                                  va_arg(ap, void *), va_arg(ap, int *));
        break;
    case 210: /* SYS_shutdown */
        ret = network::shutdown(va_arg(ap, int), va_arg(ap, int));
        break;
    case 61: /* SYS_getdents64 */
        ret = files::getdents(va_arg(ap, int), va_arg(ap, void *),
                              va_arg(ap, size_t));
        break;
    case 62: /* SYS_lseek */
        ret = files::lseek(va_arg(ap, int), va_arg(ap, long), va_arg(ap, int));
        break;
    case 71: /* SYS_sendfile: how libc++'s copy_file moves a file on Linux */
        ret = files::sendfile(va_arg(ap, int), va_arg(ap, int),
                              va_arg(ap, long *), va_arg(ap, size_t));
        break;
    case 63: /* SYS_read */
        ret = sys_read(va_arg(ap, int), va_arg(ap, void *), va_arg(ap, size_t));
        break;
    case 67: /* SYS_pread64: musl's pread, and the call LLVM's MemoryBuffer
              * reads a file it does not map through (specs/clang-on-aegir.md) */
        ret = files::pread(va_arg(ap, int), va_arg(ap, void *), va_arg(ap, size_t),
                           va_arg(ap, long));
        break;
    case 68: /* SYS_pwrite64 */
        ret = files::pwrite(va_arg(ap, int), va_arg(ap, void const *), va_arg(ap, size_t),
                            va_arg(ap, long));
        break;
    case 79: /* SYS_newfstatat: the stat family on riscv64 (musl's kstat path) */
        ret = files::newfstatat(va_arg(ap, int), va_arg(ap, char const *),
                                va_arg(ap, void *), va_arg(ap, int));
        break;
    case 80: /* SYS_fstat */
        ret = files::fstat(va_arg(ap, int), va_arg(ap, void *));
        break;
    case 291: /* SYS_statx: LLVM's fs::status calls it directly, where musl's own
               * stat family is the kstat path above, so a linker's existence check
               * lands here (files.h's statx, specs/clang-on-aegir.md). */
        ret = files::statx(va_arg(ap, int), va_arg(ap, char const *), va_arg(ap, int),
                           va_arg(ap, unsigned int), va_arg(ap, void *));
        break;
    case 93: /* SYS_exit: end the calling thread, not the process. A thread
              * that musl's __pthread_exit has finished with suspends here; the
              * process's boot thread (no TCB of its own) reports the status and
              * ends the process. */
        if (g_self_tcb != 0) {
            /* The kernel's CLONE_CHILD_CLEARTID: a thread's exit clears the
             * address musl gave clone. The clear alone releases musl's
             * spinners (__wait, the joiner's __tl_sync), which is all a
             * futex-less Aegir needs. */
            if (g_clear_tid != nullptr) {
                *g_clear_tid = 0;
            }
            seL4_TCB_Suspend(g_self_tcb);
            aegir::halt();
        } else {
            report_exit(static_cast<int>(va_arg(ap, long)));
        }
        break;
    case 135: { /* rt_sigprocmask: Aegir delivers no signals, so the mask is
                 * never changed and always empty. Succeed as a no-op -- musl's
                 * thread and cancellation setup calls this, and -ENOSYS there
                 * breaks a syscall it is bracketing (a write, in the
                 * compiler's first case; specs/cxx.md's surface). */
        static_cast<void>(va_arg(ap, int));
        static_cast<void>(va_arg(ap, void *));
        void *const oldset = va_arg(ap, void *);
        size_t const sigset_size = va_arg(ap, size_t);
        if (oldset != nullptr && sigset_size >= sizeof(unsigned long)) {
            *static_cast<unsigned long *>(oldset) = 0;
        }
        ret = 0;
        break;
    }
    case 94: /* SYS_exit_group: end the process */
        report_exit(static_cast<int>(va_arg(ap, long)));
        break;
    case 260: { /* SYS_wait4: wait for a child the launcher started
                 * (specs/posix.md). The launcher holds the reply until the child
                 * ends, so this blocks as wait4 should. */
        int const pid = va_arg(ap, int);
        int *const status = static_cast<int *>(va_arg(ap, void *));
        static_cast<void>(va_arg(ap, int));     /* options */
        static_cast<void>(va_arg(ap, void *));  /* rusage */
        ret = wait_child(pid, status);
        break;
    }
    default:
        break;
    }
    va_end(ap);
    return ret;
}

}  // namespace aegir::heap
