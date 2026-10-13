/*
 * aegir-posix-conformance: the scratch volume and the suite's small text
 * helpers.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The file and directory areas write to the disk's SCRATCH volume through the
 * POSIX view, the volume the tree already treats as its scratchpad, so the
 * AEGIR volume's own content is untouched.
 */

#ifndef AEGIR_POSIX_CONFORMANCE_SCRATCH_H
#define AEGIR_POSIX_CONFORMANCE_SCRATCH_H

#include <stdbool.h>
#include <stddef.h>

namespace aegir_posix_conformance {

constexpr char const *kScratch = "/SCRATCH";
constexpr char const *kDir = "/SCRATCH/POSIX.CONF";
constexpr char const *kFile = "/SCRATCH/POSIX.CONF/FILE.TXT";
constexpr char const *kSecond = "/SCRATCH/POSIX.CONF/SECOND.TXT";
constexpr char const *kRenamed = "/SCRATCH/POSIX.CONF/RENAMED.TXT";
constexpr char const *kAbsent = "/SCRATCH/POSIX.CONF/NO-SUCH-FILE.TXT";
constexpr char const *kFifo = "/SCRATCH/POSIX.CONF/FIFO";
constexpr char const *kText = "the POSIX conformance suite wrote this through the view\n";

/* Wait for the scratch volume to be registered, and report whether it was. The
 * disk is scanned by partmgr, which the device manager starts asynchronously and
 * which can finish after the boot services are already running: measured, the
 * suite reached its file area before `BD0Part2 "SCRATCH"` was printed, and every
 * case failed with `SCRATCH:...: no such volume`. The wait is bounded so a
 * genuine wedge is a failure the runner reports rather than an endless wait. */
bool wait_for_scratch();

/* A length and a comparison of our own: `<string.h>` cannot be included beside
 * libsel4's `sel4/arch/syscalls.h`, which declares `strcpy` with C++ linkage and
 * collides with musl's (measured: "declaration of 'strcpy' has a different
 * language linkage"). */
size_t length_of(char const *text);
bool equals(char const *a, char const *b, size_t n);

/* Whether `path` names nothing: a name that was removed has to read as absent,
 * not as a handle that outlived it. */
bool absent(char const *path);

}  // namespace aegir_posix_conformance

#endif
