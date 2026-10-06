# performance: known bottlenecks, accepted for now

Status: living (2026-10). Aegir's design notes say *what* the system is for and
`specs/*.md` records the decisions; this file records the places where the
implementation is knowingly slower than the design wants, so a shortcut taken to
unblock a phase is written down rather than rediscovered. An entry is not a bug:
it is a cost someone chose, with the fix that removes it and the spec whose phase
will.

The rule for adding one is the project's own: land the small certain piece rather
than the whole piece, but say which piece was deferred and why, here, in the same
commit. An entry that is fixed is **deleted**, not marked done -- the history is
the git log, and a file of stale entries is a file nobody reads.

## Format

Each entry names:

- **Where** -- the file and symbol, so it can be found.
- **Cost** -- the measured or reasoned cost, in a unit that can be argued about.
- **Why accepted** -- what it unblocks, and what the correct shape needs first.
- **Fix** -- the change that removes the cost.
- **Status** -- the phase or spec that will do the fix.

## The break source is polled at each input read

- **Where** `libs/hosted/aegir-heap/src/heap.cc`, `break_pending()`, called at
  the head of `sys_read`.
- **Cost** one non-blocking kernel entry (`seL4_NBRecv`) per input read -- one
  per `read(2)`/`getline`, none per output write. A program that never reads is
  never polled. It is not a spin: the poll is made only where the program is
  already entering a syscall.
- **Why accepted** the correct shape is to have the break **delivered** on a
  notification the process is already waiting on, so no poll is needed and a
  process blocked in a receive is woken. A thread has one bound notification,
  and for a GUI program the console's event channel owns it (`aegir-console`
  creates `slice->events`, the toolkit binds it, `application.cc:241`), so the
  spawner cannot bind a second one -- the first attempt faulted every GUI
  program with `TCB BindNotification: TCB already has a bound notification`.
  Delivering on the existing bound notification instead means the **child**
  supplies the source after it stands up (handing the registry a badged copy of
  the notification it binds), which needs a wire call the registry does not have
  yet. Polling is the small piece that lands the feature now; `specs/process.md`
  Phase 2 records the decision and the reason binding was dropped.
- **Fix** a process-supplies-its-source path: the runtime/toolkit gives the
  registry a badged copy of the notification it already binds, so `break` is a
  signal on that notification -- no poll, and a blocked receive is woken. The
  Poll would then remain only as the fallback for a process with no bound
  notification of its own.
- **Status** deferred past `specs/process.md` Phase 2; revisit with the enforced
  halt (Phase 3) or the terminal key (Phase 4), both of which need a process
  parked in a receive to be reached.
