# memory: the memory service

Status: decided (2026-09); the service, the runtime's source, the per-process
spawner, the limits and `Run` are landed (phases 1-5), and the generic
`aegir::launch` API (specs/launch.md Phase 2) is landed too.
Aegir's memory was, before this
spec, a set of **static partitions**: director carves fixed untypeds for
services, auth carves a fixed runtime for a terminal's spawn work, and the
spawner carves a fixed per-command untyped for each program. A program's
memory is decided before it runs, concurrency is decided by the pool, and
every growth is a constant and a manifest edit. That cannot express "a program
may take most of the machine's RAM", which a terminal -- or a desktop icon --
must.

This spec is the service that fixes it: one large pool, allocations on demand,
per-owner reclaim, and limits that are **configuration** -- `specs/limits.md`,
the system's `[section]`/`key = value` format -- so the code names no number and
the default is the machine.

## Why not capacities

The rule is *capacity tables grow on demand* (AGENTS.md), and the partitions
violate it in a way that has already cost rounds: each "no memory" was met by
enlarging a constant (the command pool 4 -> 32 MiB, the session pool 16 -> 64,
auth's delegation 32 -> 128, a per-command bracket 2 -> 4 MiB). The shape is
wrong, not the sizes: a child is handed one `untyped` at spawn and its runtime
(`libs/hosted/aegir-heap`) retypes its whole heap and page tables from it, so
its ceiling is that untyped, fixed at spawn.

seL4 gives per-process accounting and hard caps for free, but *which* caps is
policy. Aegir's are configuration and **opt-in**: by default a process is
bounded only by the machine, and an operator adds a rule when they want one
(`specs/limits.md`). The user database names a user's *class*
(`specs/auth.md`); the rules live in `Sys:S/limits.manifest`; the authority
model (`specs/authority.md`) is why there is no cap until one is asked for.

## The service

A service, `memory`, owns one large untyped covering most of the machine and
serves one port, `mem.main`. What it hands out is **pristine untyped**, not
frames: a process's runtime already retypes frames, page tables and its heap
from an untyped, so a chunk is the unit that fits it, and a chunk with nothing
derived is the one thing that can be given away (a used untyped cannot be
copied -- `seL4_RevokeFirst`).

- The pool is **the untypeds the boot did not spend**: director, when the
  memory service is spawned, carves the largest free piece less what the
  services after it still need (`memory = rest` in the manifest, `services.cc`).
  The pool is therefore the host's memory, not a constant -- more RAM is more
  pool with no edit -- and one untyped carries it. The size is the caller's: a
  command asks for its customary 2 MiB, a terminal's runtime for 4, and the
  pool splits to whichever it is.
- **`alloc` -- words: requested size bits. Answer: one capability, a chunk of
  that size, and its size in words.** The service **carves** the chunk from the
  pool then and there -- it is not pre-split into capabilities -- so what the
  service's CSpace bounds is the chunks *alive*, not the pool. The chunk is
  owned by the calling capability's **badge**. A session's whole kit -- the
  bureau's untyped, the terminal's runtime and shell pool, and the spawn's
  staging -- is asked for as it is needed and charged to the session's badge,
  so there is no pool to size and one release takes it all back.
- **`release` -- words: a badge. Answer: how many chunks were returned.** Every
  chunk the badge holds is revoked -- the caller's objects derived from it go
  with it -- and the untyped freed back to the pool. A spawner calls this for a
  child's badge when the child exits, exactly as the VFS's `reap` drops a
  badge's file handles (`specs/vfs.md`). A caller that would rather not retry
  `alloc` may be handed a context to wait on instead (`specs/signal.md`,
  Phase 4).
- **Limits are enforced at `alloc`** (`specs/limits.md`). The service knows the
  caller's badge, and a user badge carries the user index (`aegir/ipc/port.h`);
  at boot it reads `users.db` for each user's class and `Sys:S/limits.manifest`
  for the rules, and resolves the caller's `(resource, action)` by precedence.
  A `deny` refuses the allocation that crosses it; a `log` records the crossing
  and lets it through; with no rule the allocation is granted from the pool.

## The owner is the badge, minted per process

The unit of ownership is a **process**, not a session: a command's memory must
be reclaimed when *it* exits, not when the session does. So the memory port
copies are badged per process:

- The spawner mints a copy of `mem.main` badged with the new process's id and
  asks `alloc` for the child's first chunk; the child's own copy, in its
  bootstrap block, carries the same badge, so its runtime's later allocations
  are owned by the same id.
- On the child's exit the spawner calls `release(id)`, and everything the child
  asked for -- its initial objects and every chunk it grew into -- comes back in
  one call, the same whole-life reclaim the command pool gave, without the pool.

This is the whole-life reclaim `specs/shell.md`'s Phase 4 chose the command
pool for, kept, with the *memory* moving from a per-command pool to a shared
one.

## The runtime grows through it

`aegir-heap`'s allocator grows by **asking the service**, not by retyping a
fixed seed:

- The allocator gains an **untyped source**: when `refill` finds no piece, it
  calls a registered source for another untyped, adopts it, and retries. The
  source is the runtime's: it calls `mem.main`'s `alloc` and returns the chunk.
- The chunk the child is handed at spawn is its *first* chunk, not its ceiling;
  a program that wants most of RAM asks for more and gets it, up to the machine
  -- or the limits an operator set (`specs/limits.md`).

Nothing about `mmap`/`malloc` changes at the libc edge; only where the memory
comes from.

### What is missing: an `munmap` that gives the frames back

The dispatcher answers `SYS_mmap` by walking `mmap_` down and mapping fresh
frames, and `SYS_munmap` by **doing nothing** -- the comment says the pages
"stay mapped ... an mmap that follows hands back over the same memory"
(`libs/hosted/aegir-heap/src/heap.cc`). The second half is what does not hold:
`mmap_` only ever decreases, so the memory an `munmap` returns is never handed
back, and a process that turns over large allocations reaches the end of its
budget. It is invisible while those are few, which is why it went unnoticed; a
font service opening faces finds it at the sixth.

The measurements, so the next attempt starts further along:

- A face in the CJK collection is CFF-based (`CFF ` is 15.4 MB of a 19.5 MB
  `.ttc`) and opening one costs about **2.4 MB**. On the host it comes straight
  back -- peak RSS 7.2 MB, back to 5.4 MB -- so the rasterizer is not the leak.
- A naive fix was written and **reverted because it corrupts**. The list was
  kept in the released regions themselves (two words, no table to allocate),
  address-ordered with a first fit, a split and a neighbour merge, and a host
  conformance of 65 checks passed. In the heap it did not: bounded by the
  `mmap_` cursor it was *safe* but changed nothing (mallocng's releases are
  mostly of older regions, which the bound refused), and bounded by the
  region's `limit_` it accepted those older releases and **the demo and the
  service faulted on a null access**.

The trace (`-DAEGIR_HEAP_TRACE` on `aegir-heap`, read back by
`scripts/heap_trace.py`) says where the fault is **not**, which is progress:

- **mallocng's releases pair exactly.** Over a whole boot, 378 releases of 378
  name a range currently mapped by that process, with no overlap and no
  sub-range. So there is no contract violation to find in mallocng; the pairs
  the heap is handed are well-formed. (The handful the checker first called
  unknown were console-split log lines, counted and shown.)
- **The list's logic is sound for the real stream.** Replaying the logged
  `mmap`/`munmap` sequence through a faithful port of the list hands out a live
  region **zero** times.

What follows from that is where to look next: not the list's arithmetic but
**where the list is kept and what a reused region contains**. A node written
into a released region can be clobbered after the release, and a region handed
back *dirty* violates `mmap`'s promise that a mapping is zero-filled.

**Landed.** The list now keeps its nodes in memory the heap owns -- a chain of
pages mapped below the cursor, the way `Allocator` keeps its own node pool, so
growing the list never allocates through the list it is growing and a released
region is never asked to describe itself -- and a region handed back out is
zeroed first, because that is what `mmap` promises and a released region holds
its last owner's bytes. The split, the first fit and the neighbour merge are
unchanged. `scripts/check_regions.py` asserts 297 cases, and the boot the fix
was measured against is green (specs/fonts.md picks the font service back up).

## What the spawner stops doing

`SpawnKit` stops carving a per-command untyped from a per-command pool. For a
command it asks the service for a chunk owned by the command's id, retypes the
command's objects and image frames from it, and on exit calls `release`. The
command pool, the `background-pool`, and the per-command bracket sizes go away;
there is no fixed bracket, and a command is bounded only by its limits (by
default, the machine).

## Phases

- **Phase 1 -- the service and the protocol.** Landed. `memory` takes the
  boot's remaining untypeds (`memory = rest`), serves `alloc`/`release`
  carving chunks on demand, and a smoke asks for a chunk, retypes a frame from
  it, and releases. The allocator grew the shared slot pool and the free hook
  this needs. No runtime change yet.
- **Phase 2 -- the runtime's untyped source.** Landed. The allocator asks a
  registered source for another untyped when its free lists hold none, adopts
  it and retries; aegir-heap installs the source, and the source calls
  `mem.main`. The cxx-smoke client is spawned with a 256 KiB seed and
  heap-allocates past it, so its pages come from the service -- every hosted
  program given the port grows the same way.
- **Phase 3 -- the spawner and the per-process badge.** Landed. The spawner --
  the session's launcher, or the boot terminal -- mints a `mem.main` copy badged
  for each command, so the command's objects, image frames and runtime growth
  are all owned by its id and come back in one `release`; the command pool is
  gone. Auth hands the spawner the delegatable copy instead of carving a pool.
- **Phase 4 -- limits.** Landed. `specs/limits.md`: the rule parser
  (`aegir-limits`, host-tested), the shipped `Sys:S/limits.manifest`, the user
  database's `class=` field (AUDB v3), and the enforcement at `alloc`. The
  service's manifest gains `vfs.namespace`, and it reads `Initrd:users.db` and
  `Sys:S/limits.manifest` at boot, resolving each user's `(memory, log)` and
  `(memory, deny)` once. The temporary command badge is gone with it: the
  process's own badge rides in the bootstrap block, so a session terminal mints
  its commands' memory as user badges and limits apply. The default is
  unrestricted; a `deny` refuses the chunk that crosses it and a `log` reports
  it.
- **Phase 5 -- `Run`/`NewCLI`.** `Run` landed: with no per-command pool, a
  background command is one more process under its own limits, and `Run` is
  "spawn without waiting". Its exit carries the command's own badge, so the
  launcher reaps it, on the stream's word, without a `return code` line, and
  the spawner keeps a pool of live commands rather than one line's bracket. `NewCLI`/`NewShell` (a new
  Shell in a new window) is not a memory feature: it is the generic launch
  mechanism of `specs/launch.md`, whose Phase 2 -- `aegir::launch` over the
  runtime primitive, and the shell's command lines, `Run` and pipelines routed
  through it -- has landed.

## What this is not

- **A pager or swap.** The pool is RAM; there is no backing store.
- **A general frame broker.** What a process gets is untyped it retypes from;
  a device's frames stay the device manager's (`specs/services.md`).
- **A complete policy engine.** One resource, two actions and three subjects
  are the first cut; per-service and per-session subjects, a growth a supervisor
  confirms (`confirm`), and `signal`/`throttle` are later (`specs/limits.md`).
