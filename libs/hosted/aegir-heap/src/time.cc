/*
 * The runtime's clock and sleep calls: what musl's clock_gettime and nanosleep
 * reach (specs/cxx.md, specs/timer.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Wall-clock time is the clock service's (aegir/clock.h); the monotonic clock
 * and the sleep are the timer's (aegir/timer.h), because on real hardware they
 * are different devices and a port per question lets each be ported on its own.
 * Both ports are found by name through the bootstrap block, like the namespace
 * (files.cc): a process not given one in its manifest `needs` has no clock, and
 * asks are refused rather than invented.
 */

#include "time.h"

#include <aegir/clock.h>
#include <aegir/ipc/port.h>
#include <aegir/timer.h>
#include <errno.h>

namespace aegir::heap::time {

/* CLOCK_REALTIME and CLOCK_MONOTONIC, the two a program asks for without a
 * timer (musl's <time.h> numbers them 0 and 1 on Linux). */
constexpr int kClockRealtime = 0;
constexpr int kClockMonotonic = 1;

namespace {

/* One `now` from the named port, into the caller's struct timespec. */
long now_from(char const *name, uint32_t name_length, uint32_t method,
              uint32_t words, void *tp) noexcept
{
    aegir::ipc::Consumer const source = aegir::ipc::Consumer::find(name, name_length);
    if (!source.valid()) {
        return -ENOSYS;
    }
    uint64_t answer[aegir::clock::kNowWords] = {};
    aegir::ipc::WordsReply const reply =
        source.call_words(method, nullptr, 0, answer, words);
    if (reply.error != 0 || reply.count < words) {
        return -EIO;
    }
    /* struct timespec: tv_sec then tv_nsec, both machine longs (musl rv64). */
    auto *timespec = static_cast<long *>(tp);
    timespec[0] = static_cast<long>(answer[0]);
    timespec[1] = static_cast<long>(answer[1]);
    return 0;
}

}  // namespace

long clock_gettime(int clock_id, void *tp) noexcept
{
    if (clock_id != kClockRealtime && clock_id != kClockMonotonic) {
        return -EINVAL;
    }
    if (tp == nullptr) {
        return -EFAULT;
    }
    if (clock_id == kClockMonotonic) {
        long const reading =
            now_from(aegir::timer::kPortName, aegir::timer::kPortNameLength,
                     aegir::timer::kMethodNow, aegir::timer::kNowWords, tp);
        if (reading != -ENOSYS) {
            return reading;
        }
        /* A process the session did not give the timer -- a boot service, or a
         * command started outside one -- still has the clock, which never runs
         * backward, so a monotonic reading falls back to it rather than
         * failing (specs/timer.md). */
    }
    return now_from(aegir::clock::kPortName, aegir::clock::kPortNameLength,
                    aegir::clock::kMethodNow, aegir::clock::kNowWords, tp);
}

long nanosleep(void const *request, void *remaining) noexcept
{
    if (request == nullptr) {
        return -EFAULT;
    }
    auto const *timespec = static_cast<long const *>(request);
    if (timespec[0] < 0 || timespec[1] < 0) {
        return -EINVAL;
    }
    uint64_t const duration = static_cast<uint64_t>(timespec[0]) * 1000000000ull +
                              static_cast<uint64_t>(timespec[1]);
    aegir::ipc::Consumer const timer =
        aegir::ipc::Consumer::find(aegir::timer::kPortName, aegir::timer::kPortNameLength);
    if (!timer.valid()) {
        return -ENOSYS;
    }
    uint64_t const request_words[1] = {duration};
    uint64_t answer[1] = {};
    aegir::ipc::WordsReply const reply = timer.call_words(
        aegir::timer::kMethodSleep, request_words, 1, answer, 1);
    if (reply.error != 0 || reply.count < 1) {
        return -EIO;
    }
    /* The wait is never interrupted here, so no time is left over. */
    if (remaining != nullptr) {
        auto *left = static_cast<long *>(remaining);
        left[0] = 0;
        left[1] = 0;
    }
    return 0;
}

}  // namespace aegir::heap::time
