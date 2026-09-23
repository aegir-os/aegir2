/*
 * The clock port's protocol: what a time source serves.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the same shape as aegir/entropy.h and aegir/block.h:
 * the service includes it to serve, a client includes it to call, and neither
 * has to guess what the other meant (specs/services.md). A `now` answer is
 * two words -- whole seconds since the Unix epoch, and nanoseconds within the
 * second -- so it carries no shared window and rides in the port's own
 * envelope. There is no timer or interrupt here: the clock answers what time
 * it is, and waiting is a later protocol's job.
 */

#ifndef AEGIR_CLOCK_H
#define AEGIR_CLOCK_H

#include <stdint.h>

namespace aegir::clock {

/** The port's name, in the class.instance shape every port is named in. */
constexpr char kPortName[] = "clock.main";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

/** Now: no request words. The answer is the time since the Unix epoch as
 *  whole seconds, then nanoseconds within the second (`kNowWords` words). */
constexpr uint32_t kMethodNow = 1;
constexpr uint32_t kNowWords = 2;

}  // namespace aegir::clock

#endif  // AEGIR_CLOCK_H
