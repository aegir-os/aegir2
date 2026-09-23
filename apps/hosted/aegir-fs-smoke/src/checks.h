/*
 * aegir-fs-smoke: the hosted wrapper's checks, behind a header with no C++
 * includes.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This declaration exists so the aegir::filesystem checks can live in their
 * own translation unit (checks.cc). They include libc++ headers, which pull in
 * musl's <string.h>; the seL4 headers declare `strcpy` with C++ linkage, and
 * the two clash in one translation unit (aegir-cxx-smoke/src/checks.h says the
 * same). Keeping the seL4-facing code (main.cc) and the libc++-facing code
 * (checks.cc) apart is the rule specs/userland.md records.
 */

#ifndef AEGIR_FS_SMOKE_CHECKS_H
#define AEGIR_FS_SMOKE_CHECKS_H

namespace aegir::fs_smoke {

/** Run the aegir::filesystem wrapper's checks, writing each result to the
 *  debug console. Returns the number that failed. */
int run();

}  // namespace aegir::fs_smoke

#endif  // AEGIR_FS_SMOKE_CHECKS_H
