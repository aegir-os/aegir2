# The timer

Aegir's interval time: a monotonic clock and a sleep, served as `timer.main`
(`aegir/timer.h`). It is deliberately separate from `clock.main`
(`aegir/clock.h`), which answers wall-clock time. The two are different
questions and, on real hardware, different devices -- an RTC answers what time
it is, a platform timer measures an interval -- so a port per question means
each is ported on its own. The timer's epoch is unspecified: a caller subtracts
two readings, never interprets one.

## The shape

`timer.main` serves three methods:

- `now`: no request words; the answer is the monotonic time as whole seconds,
  then nanoseconds within the second. Two readings bound an interval.
- `sleep`: one request word, a duration in nanoseconds; the answer is one word,
  1 when the wait completed. A caller waiting until a time computes the
  duration from a `now` reading.
- `subscribe`: one request word, a period in nanoseconds, and one capability
  riding beside it -- the caller's notification, which the timer signals each
  period. The answer is one word, 1 when the subscription is taken. It is the
  clock edge a service that waits on the world needs and cannot poll for; the
  tcpip thread's timed fetch is its first user (`specs/net.md`).

A caller finds the port by name through its bootstrap block, the way it finds
`clock.main` (specs/services.md). The hosted runtime answers
`clock_gettime(CLOCK_MONOTONIC)` and `nanosleep` through it; the wall-clock
calls stay `clock.main`'s.

## Waiting on two sources

A tick is not a call, and the two arrive on one receive. The service binds the
interrupt's notification to its serving thread, so `seL4_Recv` on the port sees
a call and a tick alike (`specs/signal.md`). Telling them apart is the badge: a
call carries the caller's badge, and a *system* caller's is 0 -- the same as an
unbadged signal. So the tick's notification is a **badged copy**: the service
mints the interrupt's notification with a context bit and points the handler at
the copy (`seL4_IRQHandler_SetNotification`), so a tick's badge is exactly that
bit while badge 0 is a call. The device manager grants the notification Write,
because pointing the handler at the copy is a send right on it.

A sleep is a **held reply**: the caller's reply capability is saved and answered
when the alarm reaches its target, so the service stays free to serve `now`, a
`subscribe`, or another sleep meanwhile. Several sleeps are held at once (a held
reply each, in a pool grown from the service's region), because hosted processes
sleep independently and a refused sleep would be a silently shortened one -- the
hosted `nanosleep` never reads the answer word.

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
- **Not a queue of timers per client.** Several sleeps are held at once, and a
  subscription is a capability to signal -- not a thread and not a queue per
  client; a caller that needs many timers keeps its own bookkeeping.
- **High resolution.** The device's granularity is the wait's, not the
  nanosecond the request is written in, so a caller must not assume a sleep
  returns on the nanosecond.
