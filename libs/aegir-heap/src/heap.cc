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
 */

#include <aegir/heap.h>

#include <aegir/debug.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <errno.h>
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
    return true;
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
    case 93:  /* SYS_exit */
    case 94:  /* SYS_exit_group */
        aegir::halt();
        break;
    default:
        break;
    }
    va_end(ap);
    return ret;
}

}  // namespace aegir::heap
