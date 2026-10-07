/*
 * aegir-heap's POSIX spawn interface (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The pid table `posix_spawn` fills and `wait4` drains, kept behind a header
 * with no libc++ include: the runtime's dispatcher (heap.cc) must not see a
 * libc++ or seL4 header beside musl's, so the table and the spawn entry point
 * live in posix.cc, which may.
 */

#ifndef AEGIR_HEAP_POSIX_H
#define AEGIR_HEAP_POSIX_H

#include <stdint.h>

namespace aegir::heap {

/* Register a spawned child's badge under a new 32-bit pid, and take it back for
 * wait4 (POSIX's pid_t is 32 bits; an Aegir badge is 64). */
int register_child(uint64_t badge) noexcept;
bool take_child_badge(int pid, uint64_t *badge) noexcept;

}  // namespace aegir::heap

#endif  // AEGIR_HEAP_POSIX_H
