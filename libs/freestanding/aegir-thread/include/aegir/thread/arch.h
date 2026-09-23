/*
 * What starting a thread needs that an architecture decides.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The thread primitive is otherwise architecture-neutral: it uses the seL4
 * user context (whose fields the kernel names per architecture) and the
 * spawner's CSpace/VSpace shape. Two things are not neutral, and they live
 * here so that adding an architecture is a change to this file and not to the
 * primitive:
 *
 *   - the object the kernel calls a 4 KiB page frame, whose type name differs
 *     by architecture (seL4_RISCV_4K_Page, seL4_ARM_SmallPageObject,
 *     seL4_X86_4K);
 *   - the global pointer, which only some architectures have. RISC-V computes
 *     it in the crt from __global_pointer$; a thread started by hand skips
 *     that, so it must be told. An architecture without one returns zero.
 */

#ifndef AEGIR_THREAD_ARCH_H
#define AEGIR_THREAD_ARCH_H

#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::thread::arch {

/* One 4 KiB page of a thread's stack, as the object the allocator retypes. */
#if defined(__riscv)
constexpr seL4_Word kPageObject = seL4_RISCV_4K_Page;
#elif defined(__aarch64__)
constexpr seL4_Word kPageObject = seL4_ARM_SmallPageObject;
#elif defined(__x86_64__)
constexpr seL4_Word kPageObject = seL4_X86_4K;
#else
#error "aegir-thread: no 4 KiB page object type for this architecture"
#endif

/* The global pointer a new thread must inherit, or zero on an architecture
 * that has none. */
inline seL4_Word global_pointer() noexcept
{
#if defined(__riscv)
    seL4_Word gp = 0;
    asm volatile("mv %0, gp" : "=r"(gp));
    return gp;
#else
    return 0;
#endif
}

}  // namespace aegir::thread::arch

#endif  // AEGIR_THREAD_ARCH_H
