# signal: readiness without polling

Status: decided (2026-09). The primitive, the held reply, and where each lands.
This supersedes the session doorbell (`specs/launch.md`, `specs/terminal.md`)
and the terminal-to-launcher relay; it is step 2 of the four in
`specs/direction.md`.

A service that must wait for the world -- for a key, a frame, an interrupt, a
release -- has to be woken, not spun. Aegir builds that wakeup by hand in every
place it needs one, in three different shapes. This spec is the one shape.

## The rules it is built on

Two kernel facts bound what any design can do, and both are already written
down in the tree:

- **A thread has one blocked receive.** A server cannot receive on its port and
  on a second object at once; that is why the console chose a notification plus
  a ring over an endpoint per window (`specs/console.md`), and why every serving
  supervisor binds one notification to its serving thread
  (`specs/services.md`, `kernel/manual/parts/ipc.tex`'s one-blocked-receive
  rule).
- **A thread-to-thread message carries one capability.** The receiver names a
  *single* receive slot: "receiving threads may specify only one receive slot,
  whereas a sending thread may include multiple capabilities in the message"
  (`kernel/manual/parts/ipc.tex:160`). The send ceiling is three
  (`kernel/libsel4/include/sel4/constants.h:54`, `seL4_MsgExtraCapBits = 2`),
  but the receive side is one, so a wakeup capability can never ride *beside* a
  session capability in one call.

Together they force the design: **readiness is multiplexed on one object with
tagged bits, and a server waits on its port and that one object together.**

## The idiom Aegir already builds by hand

Three services already implement this same shape, independently:

- **The console's event channel** (`specs/console.md`). One notification per
  client -- the client waits -- and a ring in the mapped slice. Console's own
  side of the drivers is the same shape: `subscribe` hands each driver a minted,
  badged notification, and a signal wakes the receive that serves the port.
- **virtio-input's held reply** (`specs/services.md`). `next` answers "not yet"
  by *holding the caller's reply capability* (`seL4_CNode_SaveCaller`) and
  answering when the interrupt lands. This is the completed form: the caller
  waits inside the call, and the server stays free.
- **`con.stream`'s doorbell** (`specs/terminal.md`). The client passes a
  notification at `open`, and the handler rings it when a line is ready.

Each is correct; each is rebuilt from raw seL4; each is subtly different. The
cost is not the mechanism -- it is that a service author writes it again, and
that the wakeup's *ownership* drifts: the doorbell became something the spawn
graph hands out (auth made it, the launcher embedded it) rather than something
the client registers with the service it is waiting on.

## The primitive

One library, `aegir::signal` (`libs/aegir-signal`, `aegir/signal.h`):

- **`Context`** -- a waitable source. A receiver allocates one, and it owns a
  badge bit of the receiver's notification, so a signal is a tagged wakeup and a
  coalesced repeat loses nothing (identities, not counts).
- **`Context_capability`** -- a minted, badged capability to a context, handed
  to whoever must signal it. This is console's `listen` and the drivers'
  `subscribe`, given a name.
- **`Receiver`** -- owns the one notification and a set of contexts. `wait()`
  answers which contexts are ready; `notification()` is the object a server
  binds to its port so one receive sees calls and signals.
- **`Transmitter`** -- the client side: signal a `Context_capability`.
- **`Reply_holder`** -- saves a call's reply capability (invokes
  `seL4_CNode_SaveCaller`) and answers later, optionally carrying a capability
  (`kernel/manual/parts/ipc.tex`, "Calling and Replying"). Used when the answer
  is "later", not "not yet".

The library is policy-free: it does not decide who may signal whom. A service
decides what a context means, and the capability is revoked when the session
that handed it out closes, because it was minted from the client's own
notification.

## The held reply is what a blocking read is

`con.stream`'s `read` is defined today as a poll with a doorbell beside it: the
client passes a notification at `open`, and the handler rings it when there is
something to read (`specs/terminal.md`). The doorbell exists only because the
*reply* cannot be held.

It can. virtio-input already holds a reply (`specs/services.md`). So:

- **`read` becomes a held reply.** The handler, finding nothing queued and no
  exit pending, saves the caller's reply capability and answers when a key
  arrives or the command ends. A client that will wait gets its bytes, not a
  zero; the tier-1 poll remains for a caller that will not.
- **The doorbell goes away.** There is no notification to make, none to hand
  out, and -- the point -- none for the spawn graph to carry: a launch needs the
  stream capability and nothing else (`kernel/manual/parts/ipc.tex:160`).
- **Focus stops being inferred from spawning.** A held raw read *is* "this
  stream has a reader": while it is held, a key typed at the console belongs to
  that stream's input queue. The terminal no longer needs `begin_command` --
  the spawn-inferred state the launcher arc discovered it had -- because the
  pending read is the state.

## The line, announced by the shell

One piece of a line is not in the stream and cannot be read off it: how many
stages it has. The terminal prints `pipeline exited` apart from `command
exited`, and it cannot count stages it did not spawn.

The shell owns the line, so the shell announces it: a `con.stream` method
(`line`) that carries the stage count when a line begins, before its first
command is launched. The terminal then holds the stage count and the bracket,
and reports the completion cue exactly as it does now. This is the whole of the
terminal's remaining line knowledge -- one word, from the component that owns
the line, instead of a spawn the terminal no longer performs.

## Where it lands

1. **The library and its tests.** `Receiver`, `Context`, `Reply_holder`,
   `Transmitter`; the console's and virtio-input's hand-rolled copies are
   rewritten on it, which is how the API is proved -- two independent
   implementations, one of which already works.
2. **`con.stream`.** `read` holds its reply; the doorbell is struck from the
   protocol, from `aegir-spawn-kit`'s `command_ports`, and from auth (the
   session doorbell); the `line` method is added.
3. **The launcher.** The shell calls `launch.session` directly (the recorded
   decision); the launcher hands a command the caller's stream capability, which
   is the request's only capability. The terminal-to-launcher relay is dropped:
   the terminal learns the stream's state from the stream, not from who spawned
   whom.
4. **The rest.** `mem.main`'s release may publish a context so a waiting
   allocator is woken rather than retried; the drivers' interrupt notifications
   and the console's event channel become the same library.

## Phases

- **Phase 1 -- the library.** The primitive and its tests, with the console's
  event channel and virtio-input's held reply rewritten on it. No wire changes.
- **Phase 2 -- the stream.** `read` holds its reply; the `line` method is added;
  the doorbell is removed end to end. This is the piece the launcher arc waits
  on.
- **Phase 3 -- the launcher.** The shell calls `launch.session`; the relay is
  removed.
- **Phase 4 -- the field.** IRQ wakeups, `mem.main` release, the console's slice
  events, all on the library.

## What this is not

- **Not threads.** A held reply keeps a service single-threaded and costs one
  saved capability; a worker per blocked call costs a TCB and a stack and does
  not compose with the one-blocked-receive rule.
- **Not POSIX signals.** There is no asynchronous interruption of a running
  thread; a context is a waitable source, and the thread chooses when to wait.
- **Not a replacement for ports.** A call is still a call; this is the other
  half -- how a waiting answer is delivered, and how a set of sources is waited
  on together.
- **Not "defer everything".** A reply is held only when the answer genuinely
  waits on the world. A call that can answer answers.
