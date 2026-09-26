/*
 * The timer port's protocol: a monotonic clock and a sleep.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the same shape as aegir/clock.h: the service includes
 * it to serve, a client includes it to call, and neither has to guess what the
 * other meant (specs/timer.md). It is deliberately *not* the wall clock. The
 * two are different questions and, on real hardware, different devices -- an
 * RTC answers what time it is, a platform timer measures an interval -- so a
 * port per question means each is ported on its own. `now`'s epoch is
 * unspecified: a caller subtracts two readings, never interprets one.
 */

#ifndef AEGIR_TIMER_H
#define AEGIR_TIMER_H

#include <stdint.h>

namespace aegir::timer {

/** The port's name, in the class.instance shape every port is named in. */
constexpr char kPortName[] = "timer.main";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

/** Now: no request words. The answer is the monotonic time as whole seconds,
 *  then nanoseconds within the second (`kNowWords` words). The epoch is the
 *  service's, not the caller's; only a difference is meaningful. */
constexpr uint32_t kMethodNow = 1;
constexpr uint32_t kNowWords = 2;

/** Sleep: one request word, the duration in nanoseconds. The answer is one
 *  word, 1 when the wait completed. A caller waiting until a time computes the
 *  duration from a `now` reading. The wait's granularity is the device's, so a
 *  caller must not assume it returns on the nanosecond. */
constexpr uint32_t kMethodSleep = 2;
constexpr uint32_t kSleepWords = 1;

}  // namespace aegir::timer

#endif  // AEGIR_TIMER_H
