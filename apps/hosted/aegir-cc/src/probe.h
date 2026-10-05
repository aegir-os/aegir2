/*
 * aegir-cc: the LLVM-facing half, behind a header with no C++ includes.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This declaration exists so the LLVM code can live in its own translation unit
 * (probe.cc). It includes libc++ and LLVM headers, which pull in musl's
 * <string.h>; the seL4 headers declare `strcpy` with C++ linkage
 * (kernel/libsel4/arch_include/riscv/sel4/arch/syscalls.h:837), and the two
 * clash in one translation unit. Keeping the seL4-facing code (main.cc) and the
 * libc++-facing code (probe.cc) apart is the rule specs/userland.md records.
 */

#ifndef AEGIR_CC_PROBE_H
#define AEGIR_CC_PROBE_H

namespace aegir::clang_probe {

/** Read `path` through `llvm::MemoryBuffer` and report its size on the console.
 *  Returns 0 on success, nonzero on failure. This is the first measurement of
 *  specs/clang-on-aegir.md: it reads a file the way clang reads a source, over
 *  the runtime's POSIX surface, so whatever LLVM's file layer needs that Aegir
 *  does not yet answer shows up here. */
int read_file(char const *path);

/** Write a small freestanding source to `source_path`, compile it with clang's
 *  frontend and codegen in this process into `object_path`, and report the
 *  object's size on the console. Returns 0 on success, nonzero on failure. This
 *  is the first real compile: the frontend half of the in-process driver
 *  (specs/clang-on-aegir.md Phase 3). */
int compile(char const *source_path, char const *object_path);

}  // namespace aegir::clang_probe

#endif  // AEGIR_CC_PROBE_H
