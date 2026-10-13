/*
 * aegir-posix-conformance: the time area.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Every case is derived from the page named beside it in
 * third_party/posix-spec/issue8 (fetched by `make deps`). gettimeofday and
 * settimeofday are not in the surface: their pages are 404, because Issue 8
 * replaced them with clock_gettime/clock_settime.
 */

#include "harness.h"
#include "scratch.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

namespace aegir_posix_conformance {
namespace {

bool is(char const *text, char const *want)
{
    size_t const n = length_of(want);
    return equals(text, want, n) && text[n] == '\0';
}

void conforms_or_deviation(char const *interface, char const *test, char const *clause,
                           bool conforms, int observed_errno, char const *expected)
{
    if (conforms) {
        check(interface, test, true);
    } else {
        char reason[128];
        snprintf(reason, sizeof(reason), "%s names %s; Aegir answers errno %d", clause, expected,
                 observed_errno);
        deviation(interface, test, reason);
    }
}

}  // namespace

void time_surface()
{
    section("time");

    /* clock_gettime.html ERRORS: "[EINVAL] The clock_id argument does not
     * specify a known clock." */
    struct timespec now;
    check("clock_gettime", "a known clock answers", clock_gettime(CLOCK_REALTIME, &now) == 0);
    errno = 0;
    int const bad_clock = clock_gettime(-1, &now);
    int const bad_clock_errno = errno;
    conforms_or_deviation("clock_gettime", "an unknown clock is EINVAL", "clock_gettime.html",
                          bad_clock == -1 && bad_clock_errno == EINVAL, bad_clock_errno, "EINVAL");

    /* clock_getres.html ERRORS: the same EINVAL clause. */
    struct timespec resolution;
    errno = 0;
    int const getres = clock_getres(CLOCK_REALTIME, &resolution);
    int const getres_errno = errno;
    conforms_or_deviation("clock_getres", "a known clock answers its resolution",
                          "clock_getres.html", getres == 0, getres_errno, "zero");

    /* clock_settime.html ERRORS: EINVAL (unknown clock, CLOCK_MONOTONIC, a
     * nanosecond out of range), EPERM. */
    errno = 0;
    int const set = clock_settime(CLOCK_REALTIME, &now);
    int const set_errno = errno;
    conforms_or_deviation("clock_settime", "setting the clock answers", "clock_settime.html",
                          set == 0, set_errno, "zero");
    errno = 0;
    int const set_mono = clock_settime(CLOCK_MONOTONIC, &now);
    int const set_mono_errno = errno;
    conforms_or_deviation("clock_settime", "CLOCK_MONOTONIC is EINVAL", "clock_settime.html",
                          set_mono == -1 && set_mono_errno == EINVAL, set_mono_errno, "EINVAL");

    /* clock_nanosleep.html ERRORS: EINVAL, ENOTSUP, EINTR; RETURN VALUE: zero
     * when the time elapsed, or the error number itself (not -1/errno). */
    struct timespec none = {0, 0};
    int const slept = clock_nanosleep(CLOCK_REALTIME, 0, &none, nullptr);
    check("clock_nanosleep", "a zero sleep returns zero", slept == 0);
    {
        struct timespec too_many = {0, 1000000000};
        int const out_of_range = clock_nanosleep(CLOCK_REALTIME, 0, &too_many, nullptr);
        check("clock_nanosleep", "a nanosecond of 1000 million is EINVAL",
              out_of_range == EINVAL);
    }

    /* nanosleep.html: musl implements nanosleep as __clock_nanosleep, which
     * reaches SYS_clock_nanosleep -- the same sleep, and the case that proves
     * the 115 handler answers it. */
    errno = 0;
    check("nanosleep", "a zero sleep returns zero", nanosleep(&none, nullptr) == 0);

    /* sleep.html defines no error; RETURN VALUE is the unslept seconds. */
    check("sleep", "a zero sleep returns zero", sleep(0) == 0);

    /* time.html RETURN VALUE. */
    time_t first = time(nullptr);
    time_t second = 0;
    check("time", "the time is answered", first != (time_t)-1 && time(&second) != (time_t)-1);

    /* timer_create.html ERRORS: EAGAIN, EINVAL, ENOTSUP; the other timer calls'
     * pages: EINVAL. A timer to test them against is what timer_create owes. */
    timer_t timer = 0;
    errno = 0;
    int const created = timer_create(CLOCK_REALTIME, nullptr, &timer);
    if (created != 0) {
        int const create_errno = errno;
        unavailable("timer_create", "is not implemented", true, create_errno);
        unavailable("timer_delete", "is not implemented", true, create_errno);
        unavailable("timer_settime", "is not implemented", true, create_errno);
        unavailable("timer_gettime", "is not implemented", true, create_errno);
        unavailable("timer_getoverrun", "is not implemented", true, create_errno);
    } else {
        struct itimerspec when = {{0, 0}, {0, 0}};
        struct itimerspec got = {{0, 0}, {0, 0}};
        check("timer_settime", "a zero timer is set", timer_settime(timer, 0, &when, nullptr) == 0);
        check("timer_gettime", "the timer is read back", timer_gettime(timer, &got) == 0);
        check("timer_getoverrun", "the overrun count is read", timer_getoverrun(timer) >= 0);
        check("timer_delete", "the timer is deleted", timer_delete(timer) == 0);
    }

    /* The broken-down time, on a wall-clock value: gmtime.html/localtime.html
     * return a struct tm or a null pointer, mktime.html converts back,
     * strftime.html formats, ctime.html/asctime.html are strings, difftime.html
     * is a difference. */
    struct tm *const utc = gmtime(&first);
    check("gmtime", "a time is broken down", utc != nullptr && utc->tm_year >= 70);
    struct tm *const local = localtime(&first);
    check("localtime", "a time is broken down locally", local != nullptr);
    if (local != nullptr) {
        struct tm copy = *local;
        check("mktime", "localtime round-trips through mktime", mktime(&copy) == first);
    } else {
        check("mktime", "localtime round-trips through mktime", false);
    }
    {
        time_t const epoch = 0;
        struct tm *const at_epoch = gmtime(&epoch);
        char formatted[64];
        if (at_epoch != nullptr) {
            size_t const n = strftime(formatted, sizeof(formatted), "%Y", at_epoch);
            check("strftime", "the epoch formats as 1970", n == 4 && is(formatted, "1970"));
        } else {
            check("strftime", "the epoch formats as 1970", false);
        }
    }
    check("ctime", "a string is answered", ctime(&first) != nullptr);
    if (utc != nullptr) {
        check("asctime", "a string is answered", asctime(utc) != nullptr);
    } else {
        check("asctime", "a string is answered", false);
    }
    check("difftime", "a difference is answered", difftime(10, 0) == 10.0);

    /* clock_getcpuclockid.html: returns an error number, not -1/errno. */
    clockid_t cpu_clock = 0;
    int const cpu = clock_getcpuclockid(0, &cpu_clock);
    unavailable("clock_getcpuclockid", "is not implemented", cpu != 0, cpu);
}

}  // namespace aegir_posix_conformance
