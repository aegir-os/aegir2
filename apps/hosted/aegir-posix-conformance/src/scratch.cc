/*
 * aegir-posix-conformance: the scratch volume and the suite's small text
 * helpers.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "scratch.h"

#include <errno.h>
#include <time.h>
#include <unistd.h>

namespace aegir_posix_conformance {

size_t length_of(char const *text)
{
    size_t n = 0;
    while (text[n] != '\0') {
        ++n;
    }
    return n;
}

bool equals(char const *a, char const *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

bool absent(char const *path)
{
    errno = 0;
    return access(path, F_OK) == -1 && errno == ENOENT;
}

bool wait_for_scratch()
{
    /* A minute, in steps small enough that the volume is noticed as soon as it
     * appears: 300 x 200 ms. Not a limit on the volume's size or the number of
     * files -- only on how long a *missing* volume is waited for, so a wedge is
     * a failure and not an endless wait. */
    constexpr int kTries = 300;
    struct timespec pause;
    pause.tv_sec = 0;
    pause.tv_nsec = 200 * 1000 * 1000;
    for (int attempt = 0; attempt < kTries; ++attempt) {
        if (access(kScratch, F_OK) == 0) {
            return true;
        }
        (void)nanosleep(&pause, nullptr);
    }
    return false;
}

}  // namespace aegir_posix_conformance
