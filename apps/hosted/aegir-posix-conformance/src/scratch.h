/*
 * aegir-posix-conformance: the volumes the file and directory areas run
 * against, and the suite's small text helpers.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The expected result of some cases is a property of the volume, not of Aegir:
 * `chmod` succeeds where the filesystem keeps mode bits (BFS's Protect) and is
 * refused where it does not (FAT). So the areas run against *both* a FAT and a
 * BFS volume and say which is which, rather than testing one and generalising.
 */

#ifndef AEGIR_POSIX_CONFORMANCE_SCRATCH_H
#define AEGIR_POSIX_CONFORMANCE_SCRATCH_H

#include <stdbool.h>
#include <stddef.h>

namespace aegir_posix_conformance {

struct Volume {
    char const *label; /* how the suite names it in the log */
    char const *root;  /* the POSIX view's path to it, e.g. "/SCRATCH" */
    bool has_modes;    /* whether chmod/fchmod can succeed on it */
};

/* Both are writable partitions of the same disk, and both are reached through
 * the POSIX view. `SCRATCH:` is FAT; `BFS:` is the Be File System. */
extern Volume const kFatVolume;
extern Volume const kBfsVolume;

/* The text the write cases put in a file and read back. */
constexpr char const *kText = "the POSIX conformance suite wrote this through the view\n";

/* Wait, bounded, for the volume to be registered. partmgr is started
 * asynchronously by the device manager, and the measurement is why this exists:
 * the suite reached its file area before `BD0Part2 "SCRATCH"` was printed, so
 * every case failed with `SCRATCH:...: no such volume`. False if it never
 * appears -- one named failure, not an endless wait. */
bool wait_for_volume(Volume const &volume);

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
