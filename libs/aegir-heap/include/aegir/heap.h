/*
 * Aegir's freeing heap: musl's malloc over our own vspace.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The C++ standard library (specs/cxx.md) is useless without an allocation
 * story, and aegir-mem's allocator is allocation-only by design
 * (specs/userland.md's deferral). The freeing heap this library provides is
 * musl's own: the vendored full musl is built with its default backend,
 * mallocng, whose groups already recycle freed chunks below its mmap
 * threshold. What an Aegir process must supply is the memory, through the two
 * holes the seL4 musl build leaves open:
 *
 *   - musl routes *every* syscall through the `__sysinfo` function pointer
 *     (the patched riscv64 syscall_arch.h, third_party/patches/projects/musl).
 *     Aegir never sets it, so nothing syscalls today. Our dispatcher answers
 *     `SYS_brk`, `SYS_mmap`, `SYS_munmap`, `SYS_mremap` and `SYS_madvise`
 *     from pages carved out of the process's own untyped and mapped into its
 *     own vspace window.
 *   - musl's libc state is never initialised, because sel4runtime runs
 *     constructors directly and never calls `__libc_start_main`. mallocng
 *     reads `libc.page_size` for its alignment and walks `libc.auxv` looking
 *     for AT_RANDOM; both are seeded here, from the auxv sel4runtime was
 *     started with, before any allocation can ask.
 *
 * The heap's virtual budget is a slice of the address window the process was
 * granted (the aegir-mem Scratch): the caller names how many bytes, the heap
 * claims the top of the window, grows its break up from there, and serves
 * large mmaps from the space below it. Nothing is mapped until an allocation
 * needs it; freed regions are musl's to recycle, which is what makes the heap
 * *freeing* rather than a bump.
 *
 * Pages are 4 KiB. seL4's page size is what both sides agree on: musl's
 * alignment math reads `libc.page_size`, seeded here.
 */

#ifndef AEGIR_HEAP_H
#define AEGIR_HEAP_H

#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <stdint.h>

namespace aegir::heap {

/** One page, in bytes: what musl aligns to (after `libc.page_size` is
 *  seeded) and what the heap maps at a time. */
constexpr uint64_t kPageBytes = 1ull << seL4_PageBits;

/** The syscall dispatcher musl's `__sysinfo` will point at. Answers the
 *  memory syscalls from the heap's region; anything else is refused with
 *  -ENOSYS, which musl reads as errno. Declared so a closer can find it;
 *  the wiring happens at startup and in init(). */
long vsyscall(long sysnum, ...) noexcept;

/** Turn on the heap: claim `bytes` of the scratch window as the heap's
 *  virtual budget and serve musl's memory syscalls from it. Returns false
 *  when the window cannot hold `bytes` (the scratch window must already be
 *  adopted). `__sysinfo` and musl's libc state are seeded at startup, before
 *  any constructor can allocate, so everything that allocates -- operator
 *  new, malloc, the containers the standard library builds over them -- goes
 *  through the freed heap. */
bool init(aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch,
          uint64_t bytes) noexcept;

}  // namespace aegir::heap

#endif  // AEGIR_HEAP_H
