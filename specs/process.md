# process: naming a program and interrupting it

Status: proposed, for review (2026-10). This fixes the **process registry** and
the Amiga **Break** — how a running program is named by id and how it is
interrupted. It is the home `specs/dos.md` already asks for ("`Status` needs a
process registry") and the arc `specs/launch.md` defers ("a process group or job
control"). The parts it touches: `specs/terminal.md` (the key), `specs/signal.md`
(the `line` method), `specs/shell.md` (`***BREAK`, the script flag),
`specs/launch.md` (the halt) and `specs/authority.md` (the elevation check).

## The gap

Aegir cannot interrupt a running command. The pieces say so plainly:

- the shell, once a line is running, only waits for its status
  (`apps/hosted/aegir-shell/src/main.cc`: the `busy_` loop polls
  `command_status` and reads nothing else);
- the terminal hands **every** key to the running command as a byte on its
  input queue (`specs/terminal.md`), so Ctrl-C is only `0x03`, data in a stream
  the command need not read;
- `launch.session` can *reap* a command whose exit already arrived
  (`kMethodRelease`, `libs/freestanding/aegir-launch/include/aegir/launch.h`)
  and nothing else;
- and there is no asynchronous interruption to reach for: `specs/signal.md`
  is explicit — "not POSIX signals. There is no asynchronous interruption of a
  running thread".

The Amiga's answer is the one worth keeping, and it is a *command*, not a
signal.

## The Amiga's Break, kept

    BREAK <process> [NAME <program name or pattern>] [ALL | C | D | E | F]

`Break` sets **attention flags** on the process named by `<process>`:

- **C** — abort the process. The Shell then prints `***BREAK`, not a plain
  return-code line.
- **D** — halt the execution of a running *script file* (the Shell's frame).
- **E**, **F** — reserved. E was Commodity Exchange, which Aegir does not have;
  neither is given a meaning here.
- **ALL** sets C through F; the **default is C**.

That is the shape: a process is named by **id**, flags are *set* (not a message
into its stream), and the shell renders an abort distinctly. Ctrl-C at the
console is not a second mechanism — it is the default flag set on the
foreground process.

## The decisions

1. **The id is the per-process badge.** Aegir already mints one per process and
   rides it in the bootstrap block (`specs/memory.md`, "The owner is the badge,
   minted per process"); the registry's `<process>` is that badge. No second id
   is invented.
2. **Every process is named.** A spawner registers each child it starts, so the
   live set is the machine's process tree, not a set a program opts into. The
   **break source** -- an `aegir::signal` `Context` (`specs/signal.md`), the
   waitable source the process itself owns -- is the one part the child must
   provide itself, and it lands with the delivery (Phase 2).
3. **A break is a failing return code.** A process aborted by C exits with a
   distinguished nonzero status; the shell prints `***BREAK` for it and, being
   nonzero, it aborts a running script at or above `FailAt`'s level
   (`specs/shell.md`) exactly as any failing status does.
4. **Authority is the badge rule.** A caller may break a process of its **own
   class**; a process **started by the system** needs **elevation**
   (`specs/authority.md`: bit 62 is the user class — user badges carry the user
   index, system badges do not).

## The process registry

One system service, `process.registry`, holding the live set. It is the shape
`devmgr.registry` already is (`libs/freestanding/aegir-registry/include/aegir/
registry.h`): a port that answers questions, in the multi-word envelope of
`aegir::ipc`.

    count                    answer: one word, how many processes are live
    describe   index         answer: a Row's words
    break      pid, flags    answer: one word, 1 set and 0 refused
    register   metadata      in: the process's own break source as the call's
                             one capability; answer: 1 registered, 0 refused
    unregister pid           answer: one word

A `Row` is what names a process:

    pid          the process's badge, as the kernel reports it -- its identity,
                 and the class the authority check reads
    parent       the badge of the process that started it: the parent pid
    name         the program name, excluding its path
    path         the program's full path, when the launcher knew it
    flags        the attention flags currently set
    state        running, or a break pending

`break` sets the named flags in the row and *delivers* them (below). `register`
and `unregister` are the spawn side: a process appears when it is started and
leaves when it is gone. A break of a pid the registry does not hold, or one the
caller may not touch, is the refusal.

### Who registers

The **spawner** registers each child it starts, in one call
(`aegir/process_client.h`): the child's pid (the badge the spawner minted), the
spawner's own badge as the child's **parent**, and the program's name and path.
A process does not register itself, and should not have to: the spawner is the
one that knows all four, and the row is a fact about a start. So the boot
spawner registers every service, a session's spawner registers its services, a
launcher registers its commands and its nested terminals, and a terminal
registers its shell.

A shell is a process like any other, so it carries a badge of its own from the
range its terminal was delegated (`specs/launch.md`) -- not the `con.stream` key
it once shared with the logger -- and the terminal keys its stream server on that
badge. The session's serial ranges are disjoint for the same reason: a
launcher-shaped service's shell takes its own serial, and the launcher's commands
start past it, so a shell and a command never share a badge.

The capability is the spawner's: a spawner handed a `process.registry` caller
half registers its children, and one handed none registers nothing, so a Break
cannot name them. For a session, `[session.authority]` names
`process.registry` in its `needs` -- the union of what a session service may
need -- so director hands auth the unbadged `spawn:process.registry` copy, and
auth passes it to the launcher. A child never needs a registry capability of its
own unless it is itself a spawner.

The break source is the one part that must be the child's own, and the spawner
makes it too (Phase 2): it creates a notification, binds it to the child's TCB,
mints the registry's `Context` capability, grants the child its notification
capability, and passes the source capability in the `register` call -- so the
metadata and the source land together, in the one place that knows the child.

## Delivery: the flag, and the halt

The Amiga's process notices because its `Wait()` is woken and its DOS calls
return `ERROR_BREAK`. Aegir cannot wake a thread (`specs/signal.md`), so the
flag is delivered as **a source the process is waiting on, and the ultimate
halt is enforced**:

- **The source.** The registry signals the process's break `Context`
  (`Transmitter`, `specs/signal.md`). A thread has one blocked receive, so a
  process blocked in a single stream call cannot also take the signal; what the
  break reaches is the process's **idle wait** -- the runtime waits on its
  notification there, wakes on C, and exits. This is the cooperative half, and
  it is what lets a program clean up at a point it chooses. A program that never
  returns to that wait is the halt's (below).
- **The halt.** A process blocked in a *single service call* — `ping` sitting
  in the resolver — is not waiting on its break context and cannot be woken
  this way. For **C**, the abort flag, the process is *also* torn down: the
  registry asks the service that owns the process's TCB — the launcher, or the
  boot supervisor — to suspend it and release its memory by badge
  (`kMethodRelease`'s teardown, `specs/memory.md`). D/E/F are flags only; only
  C halts.

The enforced halt is what makes the feature reliable on a program that never
waits; the cooperative source is what makes it Amiga-faithful on one that does.

## The terminal and the key

Ctrl-C is the console handler's, exactly as it is on the Amiga. While a command
runs on a stream, the terminal **does not queue `0x03`** as a byte; it sets **C**
on the **foreground pid** through the registry. To do that it must know the
foreground pid, which it does not hold today (it learns a command's badge only
from its exit report, `apps/hosted/aegir-terminal/src/main.cc`).

The shell owns the line and got the badge from the spawn answer, so the shell
announces it: the `con.stream` `line` method (`specs/signal.md`, "the line,
announced by the shell"), which already carries a line's stage count, **also
carries the line's pids** — the foreground command's, and a pipeline's every
stage. The terminal then holds what it needs to break what it sees.

An interrupt reaches the **foreground** line: a background `Run` command is
untouched (the Amiga's behaviour), and a pipeline's every stage is broken, not
one.

## The shell

- When a broken command's status arrives, the shell prints **`***BREAK`** in
  place of `return code N`.
- Because the break is a failing status, it drops a running command file when it
  is at or above the fail level, precisely as `specs/shell.md`'s loop already
  does for any nonzero status.
- **D** is the flag for a script: a process that sets D on the shell's own
  command file halts the frame, which is the same act as `Quit` — the
  interpreter's word for ending a frame (`specs/shell.md`, `specs/dos.md`).

## The `Break` command

`C:Break`, a command like any other, a client of `process.registry`:

    Break <process>            set the default flag (C) on the pid
    Break <process> ALL        set C through F
    Break <process> C          set C only
    Break <process> NAME pat   break every live process whose path, or failing
                               that whose name, matches the pattern
                               (specs/pattern.md's matcher, as `Search`'s)

`NAME` matching follows the Amiga: the pattern is compared against the full
path first, then the program name. A caller that names a pid it may not break
is refused (the authority rule), and so is a pattern that matches none.

## Authority

The registry checks the caller's badge against the **target's own badge**, by
the rule `specs/authority.md` sets: a caller breaks a process of its own user
class. The class is the target's, not its starter's -- a session service is
started by auth but runs as the user, so it is the user's to break -- and a
process that **runs as the system** (a boot service, or a command an elevated
request started) is broken only by the system class, which is "the boot
services, plus whatever elevation hands out temporarily". So a session's `Break`
reaches its own processes and nothing of the system's; breaking a system process
is an elevated act, the same shape as any other (`specs/authority.md`'s
one-shot system process, not an inherited right).

## What this is not

- **Not signals.** There is no asynchronous delivery into a running thread; a
  flag is a source the process waits on, and the halt is a service taking a
  process back (`specs/signal.md` is unchanged).
- **Not a process table in the kernel.** The registry is a service; the kernel
  keeps no name for a process (`specs/authority.md`: "there is no kernel-level
  user to check against").
- **Not job control.** Stop/continue, foreground/background submission and a
  session's process *groups* are not here; this is naming and interrupting one
  process, which is the piece the shell needs first.

## Phases

1. **The registry.** Landed. The port, the wire, the row, the `Break` command
   over it, and every spawner registering the children it starts. No interruption
   yet: a row is visible, and `describe`/`count` answer (which is what `Break
   NAME` walks). This is `Status`'s ground too.
2. **The break source.** Landed for a launcher's commands. The launcher makes
   each command a notification out of the command's own memory, hands the
   command a copy through its ports (`break.source`) and a badged copy to the
   registry in the `register` call; the registry stores it against the row and
   signals it when `break` sets C; the command's runtime polls it -- one
   non-blocking syscall, made before it blocks on input -- and exits with a
   distinguished nonzero status. (The poll is a known cost, not the intended
   shape; `specs/performance.md` records it and the fix.) The source is **not
   bound** to the command's
   TCB: a thread has one bound notification, and the console's event channel
   owns it for a GUI program, so the source cannot take that slot. A thread
   already blocked inside a call is therefore not reached -- the kernel latches
   the signal (kernel/src/object/notification.c:122-131) -- and that case is the
   enforced halt's, Phase 3. The launcher **unregisters** a command as it exits,
   before the reap takes its memory back, so a stored source is never left
   pointing at a deleted capability. Still to come: the shell, nested terminals
   and the boot spawners (director, auth); and the status is not yet what the
   shell renders `***BREAK` for (Phase 4).
3. **The enforced halt.** `break` C also asks the TCB's owner to take the
   process back, so a program blocked in a single call dies.
4. **The key.** The `line` method carries the line's pids; the terminal sets C
   on Ctrl-C; the shell prints `***BREAK`; D halts a frame.

## Open, for review

- **The registry's owner.** Which system service serves `process.registry` — a
  small `aegir-process` of its own, or an existing one (the fault supervisor,
  or `auth`, which already mints identity)? Proposed: its own service, declared
  in the manifest, started by director, so nothing existing grows a second job.
- **The enforced halt's road to the TCB.** The registry holds no TCB; the
  launcher and the boot supervisor do. Whether the registry asks the TCB's
  owner by a call, or is handed a "take back" capability at registration, is
  the one mechanism this spec leaves open.
- **Registration of a freestanding service's metadata.** The kit registers it,
  but the kit's register call carries the process's own source as its one
  capability; a process with no source registers metadata alone, which needs a
  second call shape (a register with no capability). Its form is open.
- **What D means for a program that is not the shell.** The Amiga's Ctrl-D
  halts a script *file*; whether D is the shell's alone, or a general "end your
  input" a program may act on, is not fixed here.
