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

#include <aegir/heap.h>

#include <aegir/debug.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/thread.h>
#include <errno.h>
#include <sched.h>
#include <sel4/sel4.h>
#include <sel4runtime/auxv.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
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

uintptr_t align_down(uintptr_t value) noexcept
{
    return value & ~(kPageBytes - 1);
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
    return g_scratch->map_at(address, frame);
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

    /* The top of the window, page-aligned so brk arithmetic stays on page
     * boundaries, and below nothing the window already holds. */
    uintptr_t const top = align_down(scratch.limit());
    base_ = top - align_up(bytes);
    if (base_ < scratch.base()) {
        return false;
    }
    limit_ = top;
    brk_ = base_;
    mmap_ = top;
    ready_ = true;

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

/* SYS_mmap: anonymous private mappings, which is what mallocng asks for.
 * The address is ours to choose: grow down from the top of the region, past
 * the brk side. Frames are mapped at the addresses it hands out. Returns the
 * address, or a negative errno that __syscall_ret reads into MAP_FAILED. */
long sys_mmap(void *addr, size_t length, int prot, int flags, int fd,
              off_t offset) noexcept
{
    static_cast<void>(addr);
    static_cast<void>(prot);
    static_cast<void>(fd);
    static_cast<void>(offset);
    if (!ready_ || (flags & MAP_ANONYMOUS) == 0 || (flags & MAP_FIXED) != 0 ||
        length == 0) {
        return -EINVAL;
    }
    uintptr_t const needed = align_up(length);
    if (needed > mmap_ - brk_) {
        return -ENOMEM;
    }
    mmap_ -= needed;
    for (uintptr_t page = 0; page < needed / kPageBytes; ++page) {
        if (!map_page(mmap_ + page * kPageBytes)) {
            return -ENOMEM;
        }
    }
    return static_cast<long>(mmap_);
}

/* SYS_munmap: the pages stay mapped -- the address space is committed to the
 * heap, one carve we do not return -- so an mmap that follows hands back over
 * the same memory. The recycling that matters is musl's own: mallocng's
 * groups reuse the small pieces, and the large individually-mapped ones are
 * rare enough in a GUI that the virtual budget holds. Returning the frames to
 * the kernel is a later refinement, not a correctness need. */
long sys_munmap(void *addr, size_t length) noexcept
{
    static_cast<void>(addr);
    static_cast<void>(length);
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

/* SYS_madvise: a hint. mallocng asks for reclaim; the pages are already ours
 * and mapped, so there is nothing to advise. */
long sys_madvise(void *addr, size_t length, int advice) noexcept
{
    static_cast<void>(addr);
    static_cast<void>(length);
    static_cast<void>(advice);
    return 0;
}

/* SYS_write: the serial console. musl's own stdio is not wired (Aegir writes
 * through aegir-runtime), but the standard library's diagnostics do reach
 * write(), and losing them would make a terminate mute. */
long sys_write(int fd, void const *buffer, size_t length) noexcept
{
    if (fd != 1 && fd != 2) {
        return -EBADF;
    }
    auto const *bytes = static_cast<uint8_t const *>(buffer);
    for (size_t i = 0; i < length; ++i) {
        seL4_DebugPutChar(static_cast<char>(bytes[i]));
    }
    return static_cast<long>(length);
}

/* SYS_writev: what musl's stdio actually uses. Without it, vfprintf's output
 * (libc++'s verbose-abort message among it) is lost and the abort looks
 * silent. */
long sys_writev(int fd, void const *iov, int count) noexcept
{
    if (fd != 1 && fd != 2) {
        return -EBADF;
    }
    auto const *vectors = static_cast<struct iovec const *>(iov);
    long total = 0;
    for (int i = 0; i < count; ++i) {
        total += sys_write(fd, vectors[i].iov_base, vectors[i].iov_len);
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
    case 93: /* SYS_exit: end the calling thread, not the process. A thread
              * that musl's __pthread_exit has finished with suspends here; the
              * process's boot thread (no TCB of its own) halts the process. */
        if (g_self_tcb != 0) {
            /* The kernel's CLONE_CHILD_CLEARTID: a thread's exit clears the
             * address musl gave clone. The clear alone releases musl's
             * spinners (__wait, the joiner's __tl_sync), which is all a
             * futex-less Aegir needs. */
            if (g_clear_tid != nullptr) {
                *g_clear_tid = 0;
            }
            seL4_TCB_Suspend(g_self_tcb);
        }
        aegir::halt();
        break;
    case 94: /* SYS_exit_group: end the process */
        aegir::halt();
        break;
    default:
        break;
    }
    va_end(ap);
    return ret;
}

}  // namespace aegir::heap
