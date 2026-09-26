# The timer

Aegir's interval time: a monotonic clock and a sleep, served as `timer.main`
(`aegir/timer.h`). It is deliberately separate from `clock.main`
(`aegir/clock.h`), which answers wall-clock time. The two are different
questions and, on real hardware, different devices -- an RTC answers what time
it is, a platform timer measures an interval -- so a port per question means
each is ported on its own. The timer's epoch is unspecified: a caller subtracts
two readings, never interprets one.

## The shape

`timer.main` serves two methods:

- `now`: no request words; the answer is the monotonic time as whole seconds,
  then nanoseconds within the second. Two readings bound an interval.
- `sleep`: one request word, a duration in nanoseconds; the answer is one word,
  1 when the wait completed. A caller waiting until a time computes the
  duration from a `now` reading.

A caller finds the port by name through its bootstrap block, the way it finds
`clock.main` (specs/services.md). The hosted runtime answers
`clock_gettime(CLOCK_MONOTONIC)` and `nanosleep` through it; the wall-clock
calls stay `clock.main`'s.

## The hardware

On QEMU's virt machine there is no timer device a service may own: the kernel
keeps the CLINT, and the only interval source in userspace is the goldfish
RTC's alarm and its interrupt (specs/services.md). So the first `timer.main`
is backed by that alarm, and it shares the RTC device with `clock.main` -- the
device manager grants each a frame and issues the timer's interrupt
(specs/services.md). A platform with a real timer device backs the same port
with that device instead, and `clock.main` keeps the RTC; nothing a client
sees changes.

## What this is not

- **A wall clock.** `Date`, `Time` and file timestamps are `clock.main`'s
  (`aegir/clock.h`); the timer's epoch is unspecified.
- **A timer for many clients at once.** The first timer service serves one
  sleep at a time: calls serialize at the port, and the service blocks on the
  interrupt while it waits. A later timer with a queue -- and a thread, or
  deferred replies -- serves many; the protocol does not change.
- **High resolution.** The device's granularity is the wait's, not the
  nanosecond the request is written in, so a caller must not assume a sleep
  returns on the nanosecond.
