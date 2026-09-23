/*
 * The runtime's clock call: what musl's clock_gettime reaches (specs/cxx.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Plain types only, so heap.cc can include this without an seL4 header of its
 * own -- the same rule files.h follows. The function is named after the
 * syscall it answers, not the libc function, because that is the layer it
 * intercepts.
 */

#ifndef AEGIR_HEAP_TIME_H
#define AEGIR_HEAP_TIME_H

namespace aegir::heap::time {

/** Answer CLOCK_REALTIME and CLOCK_MONOTONIC from the clock service
 *  (aegir/clock.h): `tp` is the caller's struct timespec, two machine longs.
 *  Returns 0, or a negative errno -- EINVAL for a clock this runtime does not
 *  serve, ENOSYS before the clock port is bound. */
long clock_gettime(int clock_id, void *tp) noexcept;

}  // namespace aegir::heap::time

#endif  // AEGIR_HEAP_TIME_H
