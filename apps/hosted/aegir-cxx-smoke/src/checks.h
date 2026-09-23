/*
 * aegir-cxx-smoke: the runtime checks, behind a header with no C++ includes.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This declaration exists so the checks can live in their own translation unit
 * (checks.cc). They include libc++ headers, which pull in musl's <string.h>;
 * the seL4 headers declare `strcpy` with C++ linkage
 * (kernel/libsel4/arch_include/riscv/sel4/arch/syscalls.h:837), and the two
 * clash in one translation unit. Keeping the seL4-facing code (main.cc) and the
 * libc++-facing code (checks.cc) apart is the rule specs/userland.md records.
 */

#ifndef AEGIR_CXX_SMOKE_CHECKS_H
#define AEGIR_CXX_SMOKE_CHECKS_H

namespace aegir::cxx_smoke {

/** Run the runtime's malloc and libc++ container checks, writing each result
 *  to the debug console. Returns the number that failed. */
int run();

}  // namespace aegir::cxx_smoke

#endif  // AEGIR_CXX_SMOKE_CHECKS_H
