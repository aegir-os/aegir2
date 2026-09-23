/*
 * The runtime's clock call: the clock service's `now` behind musl's
 * clock_gettime (specs/cxx.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The port is found by name through the bootstrap block, like the namespace
 * (files.cc): a process that was not given clock.main in its manifest `needs`
 * has no clock, and asks are refused rather than invented. Both clocks this
 * runtime answers come from the same source -- there is no cycle counter here
 * -- which is enough for a program that wants to know the time, not to
 * measure an interval to the microsecond.
 */

#include "time.h"

#include <aegir/clock.h>
#include <aegir/ipc/port.h>
#include <errno.h>

namespace aegir::heap::time {

/* CLOCK_REALTIME and CLOCK_MONOTONIC, the two a program asks for without a
 * timer (musl's <time.h> numbers them 0 and 1 on Linux). */
constexpr int kClockRealtime = 0;
constexpr int kClockMonotonic = 1;

long clock_gettime(int clock_id, void *tp) noexcept
{
    if (clock_id != kClockRealtime && clock_id != kClockMonotonic) {
        return -EINVAL;
    }
    if (tp == nullptr) {
        return -EFAULT;
    }
    aegir::ipc::Consumer const clock =
        aegir::ipc::Consumer::find(aegir::clock::kPortName, aegir::clock::kPortNameLength);
    if (!clock.valid()) {
        return -ENOSYS;
    }
    uint64_t answer[aegir::clock::kNowWords] = {};
    aegir::ipc::WordsReply const reply = clock.call_words(
        aegir::clock::kMethodNow, nullptr, 0, answer, aegir::clock::kNowWords);
    if (reply.error != 0 || reply.count < aegir::clock::kNowWords) {
        return -EIO;
    }
    /* struct timespec: tv_sec then tv_nsec, both machine longs (musl rv64). */
    auto *timespec = static_cast<long *>(tp);
    timespec[0] = static_cast<long>(answer[0]);
    timespec[1] = static_cast<long>(answer[1]);
    return 0;
}

}  // namespace aegir::heap::time
