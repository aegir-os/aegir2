/*
 * aegir-posix-conformance: the case harness.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "harness.h"

#include <errno.h>
#include <stdio.h>

namespace aegir_posix_conformance {
namespace {

int g_passed = 0;
int g_failed = 0;
int g_skipped = 0;
int g_holes = 0;
int g_deviations = 0;

}  // namespace

void section(char const *area)
{
    printf("posix: -- %s --\n", area);
    (void)fflush(nullptr);
}

void check(char const *interface, char const *test, bool ok)
{
    if (ok) {
        ++g_passed;
        printf("posix: ok      %s: %s\n", interface, test);
    } else {
        ++g_failed;
        /* The errno is part of the failure: "open: a directory O_WRONLY is not
         * EISDIR" is a sentence, "(errno 21)" is a measurement. */
        printf("posix: FAIL    %s: %s (errno %d)\n", interface, test, errno);
    }
    (void)fflush(nullptr);
}

void unavailable(char const *interface, char const *test, bool failed_call, int err)
{
    if (failed_call) {
        ++g_holes;
        printf("posix: hole    %s: %s (errno %d)\n", interface, test, err);
    } else {
        ++g_failed;
        printf("posix: FAIL    %s: %s (expected a hole, but the call succeeded)\n", interface,
               test);
    }
    (void)fflush(nullptr);
}

void deviation(char const *interface, char const *test, char const *reason)
{
    ++g_deviations;
    printf("posix: deviate %s: %s (%s)\n", interface, test, reason);
    (void)fflush(nullptr);
}

void skip(char const *interface, char const *test, char const *reason)
{
    ++g_skipped;
    printf("posix: SKIP    %s: %s (%s)\n", interface, test, reason);
    (void)fflush(nullptr);
}

int passed()
{
    return g_passed;
}

int failed()
{
    return g_failed;
}

int skipped()
{
    return g_skipped;
}

int holes()
{
    return g_holes;
}

int deviations()
{
    return g_deviations;
}

}  // namespace aegir_posix_conformance
