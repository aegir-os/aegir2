# memory: the memory service

Status: decided (2026-09); the service, the runtime's source, the per-process
spawner, the limits and `Run` are landed (phases 1-5), and the generic
`aegir::launch` API (specs/launch.md Phase 2) is landed too. Phase 6 -- the
region interface every service sees, below -- is decided and being built,
because the raw sequence it replaces is hand-rolled in seven services and the
failures land on applications that never touch it.
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

### A program's size is never bounded by the process that starts it

The starter's own pools are **floors, never ceilings**: a spawner needs memory
enough to *ask*, never enough to *hold* its child. Three walls have already
shown the shape of getting this wrong -- a run of frames laid down in one
`seL4_Untyped_Retype` met the kernel's 256-object fan-out limit; a command's
capabilities lived in a single L2 CNode and stopped at 4096; a C++ program's
global constructors allocated before its heap existed. Each is "the child's
size leaked into its starter", and the reflex -- enlarge a constant -- is the
one `AGENTS.md` forbids.

So no constant on a spawn path is a ceiling. A command's pool is sized from its
image (`ServiceKit::command_cnode_count`), not a fixed bracket; the allocator
lays a run down in chunks at the kernel's fan-out limit. The only ceiling is
`mem.main`, which is the machine. A number may be a *floor* -- a seed, a default,
a reserved minimum -- and `specs/limits.md` is the one place a deliberate cap
belongs.

One attempt at the third of those is **off**, because it stopped every session
program from spawning, and the way it failed is worth keeping. `dd182dfd` mapped a
segment's aligned bulk as 2 MiB mega pages, so the loader's capability count would
not grow with `size / 4 KiB`. Two faults, both measured on `aegir-8g-smp4` with the
dispatch trace armed (`scripts/heap_trace.py`), both mine:

- **A mega frame is not a slot of the window the frames are filled through.** That
  window is indexed by page (`spawn/process.h`'s `window_page_bits`), so the mega
  fill reported it full -- *"the window the frame is filled through is full"* -- and
  the spawn failed.
- **A mega page covers 2 MiB of *address* space, including pages a neighbouring
  segment also maps.** The kernel refused the second mapping -- *"Virtual address
  (0xa85000) already mapped"* (`decodeRISCVFrameInvocation`) -- and `populate()`
  failed with *"a segment of the program could not be mapped"*.

The result was four failed spawns at boot: the session, its terminal, its file
manager and the launcher, with only "the launcher" surviving -- so nothing ran.
The page path is what loaded every program before it, and does again
(`libs/freestanding/aegir-mem/src/child_vspace.cc` carries both reasons beside the
range). Re-enabling needs the blocks clamped to addresses no other segment owns,
and a window that can describe a mega frame; until then the loader's capability
count grows with a segment's pages, which is a *cost*, not a ceiling.

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

**And the frames are named now, for `mprotect`.** A mapping's rights change by
issuing the Map invocation again at the same address
(`kernel/manual/parts/vspace.tex:294`), and that invocation names the frame -- so
the heap keeps one `seL4_CPtr` per mapped arena page, in chunks of 512 that it maps
the first time a page in their range is mapped. Two shapes were tried first and
rejected by measurement, and neither is worth repeating: a table sized from the
whole arena at init cost the *launcher* the untyped it spawns commands with
(`spawn: FAIL no untyped for the command's runtime`), and mapping a fresh page with
the rights its `prot` asked for cost the cxx smoke a fault on a page it had mapped,
because its thread stack is `mmap`'d and written. `mmap` maps everything writable
and `mprotect` is the only thing that narrows (specs/posix.md).

## What every service sees: a region, not a capability

The memory service hands out **pristine untyped**, and that is the right unit
for a *runtime*: a process's allocator retypes its whole heap and page tables
from one untyped, so it wants the untyped. But a service that needs memory for
a *purpose* -- a framebuffer, a filesystem's frame, a virtqueue, a child's
runtime -- does not. It needs a run of frames it can map or hand on, and it
should not have to retype them itself. Today every such service hand-rolls the
same sequence

    carve_untyped(bits) -> carve_page(untyped, bits) x N -> map -> revoke -> free

and each copy must get the capability lifecycle right: a piece is handed back
with `Revoke`, never `Delete` (a delete leaves its derived caps alive); the
capability it was carved from is the allocator's, not the caller's; and the
kernel's free index lives *in the capability*, so a copied capability and the
original disagree about how much is left. Seven services carry that sequence
(`auth`, `console`, `device-manager`, `director`, `memory`, `partmgr`, the
spawn service), and the failure when one gets it wrong surfaces far away as
"0 bytes available" -- the console's slice, a demo's window.

The invariant all of that is trying to keep is one sentence:

> **A piece the allocator lists or hands out is childless, so the kernel will
> reset its free index on the next retype** (`kernel/src/object/untyped.c:182-
> 189`: `ensureNoChildren` decides the reset).

It is not in the capability, it is not checkable by a caller, and it is the one
thing that must hold. So it is the allocator's job, not the caller's:

- **The allocator makes every piece whole at every boundary it owns** -- before
  a split, before an object retype, and before a hand-out -- by revoking the
  piece's capability. A revoke deletes every capability derived from the piece,
  so the piece is childless whatever a caller did; it costs no slot and no
  retype, and it is a no-op when the piece is already whole. A caller's
  revoke/delete discipline then cannot corrupt the pool. (The hand-out half is
  landed: `carve_untyped` revokes before it gives the piece away.)
- **A service names a region, not a capability.** `aegir-mem` grows a
  `FrameRegion`: the caller asks for *N* frames of a given size and provides the
  capability array (a capacity, so no table lives in the library), and the
  region carves the untyped, retypes the frames, and on release deletes them and
  gives the piece back -- whole, because the allocator revoked it. The service
  never sees an untyped capability, and the sequence above lives in one place.
- **A caller names its CSpace once.** `adopt_slots` carries both the depth at
  which the CNode cap is reached and the CNode's radix, because a slot delete or
  revoke addresses the slot *in* the CNode, at the radix, not at the depth the
  cap was reached. Twelve owners had set only the first, so their merges deleted
  at the wrong depth and took nothing -- a piece came back spent. The allocator
  now takes both from one call, and the separate `set_cnode_size_bits` is gone.

This is `malloc`'s promise applied to a service: the caller names what it needs
and the mechanism is somebody else's job. The runtime's `mmap`/`malloc` edge
does not change; a service that needs a region stops hand-rolling one.

### The conformance must model the hand-off

The host conformance models **one CSpace and no IPC**, so it cannot see the bug
this section is about: a piece whose capability was copied to another process,
used there, and handed back. It gains a two-level model -- a service allocator
whose source is another allocator, with capabilities *copied* between them --
so a spent piece is reproduced in seconds, not a six-minute boot.

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
- **Phase 6 -- the region interface.** Decided, being built. `aegir-mem` gains
  `FrameRegion`, and the allocator revokes every piece whole at the boundaries
  it owns, so a service asks for a region rather than carving and retyping. The
  seven services that hand-roll `carve_untyped`/`carve_page` move onto it, and
  the host conformance grows the two-level hand-off model. The console's slice
  is the first caller.

## Growing the CSpace on demand

A CSpace is a tree of CNodes, and a CNode is a fixed-size kernel object: growth
means another CNode, addressed one level deeper, not a bigger one. Aegir has two
shapes. A plain process is single-level: its own-CNode cap (slot `kSlotOwnCNode`)
is a mint with **guard 0 and radix `kCNodeBits`**, so a slot *is* a plain
number, `alloc_object` names the destination CNode itself (`node_depth = 0`,
`kernel/src/object/untyped.c:113`), and a delete or revoke addresses a slot at
depth `kCNodeBits` (`kernel/src/kernel/cspace.c`'s `resolveAddressBits`). The
spawner's TCB guard (`seL4_WordBits - kCNodeBits`) is what lets a plain CPtr
reach that mint.

A process that holds many live capabilities -- the launcher, whose command pool
is the session's set of shells, demos, nested terminals and datatype classes --
gets a **two-level** CSpace instead (`Request.cspace_l1_bits != 0`):

- The root CNode has radix `l1` and guard `seL4_WordBits - l1 - l2`; the fixed
  slots and the process's own objects live in the guard-0, radix-`l2` L2 CNode
  at root slot 0, so a *plain* slot number still resolves to `(0, slot)`.
- A slot in L2 `i` is the address `(i << l2) | j`; a cap op on it names the
  guard-zero own-CNode cap, that address, and depth `l1 + l2`, and a retype
  *into* it names the L2 CNode cap (`node_index = i`, `node_depth = l1`,
  `node_offset = j`).
- `aegir-mem` carries this as `Allocator::adopt_slots_level_two` (and
  `alloc_cnode_at_l1` to retype an L2 CNode cap straight into a root slot);
  `bootstrap::cnode_l1_bits`/`cnode_bits`/`endpoint_depth` tell a process its
  shape, and every cap op in its own CSpace goes through `endpoint_depth()`
  rather than a hard-coded `cnode_bits()`.
- The launcher's `ServiceKit` gives each live command its own L2 CNode (root
  slots 1 upward), so the command pool grows with the number of live commands
  rather than running out at one CNode. The launcher's own caps stay in L2 CNode
  0, so the two never meet.
- A command's own pool is a **run** of consecutive L2 CNodes, one per ~4096
  caps its image needs (`ServiceKit::command_cnode_count`), not one CNode: an
  84 MB image is ~21,000 frame caps, past a single CNode's 4096 -- and a
  spawned command died on exactly that ("a segment could not be mapped"). The
  allocator's slot cursor is encoded, so it spans the run, and each retype names
  its CNode from the slot (`Allocator::retype_node_index`); a run never crosses
  a CNode, because one retype names one. The bound on a command is its image and
  the machine, never a single CNode.

The block carries `l1` in the CNodeBits entry's `length` field, which that entry
had never used, so a reader that knows only `cnode_bits` is undisturbed.

`kCNodeBits` (12) is both the single-level size and the L2 radix; the old
`kLauncherCNodeBits` bump is gone -- the session launcher is `l1 = 8` over
`l2 = 12`. A class is released when its open closes, so a viewer's classes give
their slots back (`specs/datatypes.md`'s phase 2f).

## The record: what was handed out, and nothing else

Every memory failure this arc had one shape: something was released, or handed out,
that the allocator never owned. `munmap` gave back a run the window could not take; the
free list's own nodes were written where nothing was mapped; a release whose length ran
past the window's end was accepted and the next `mmap` handed back the page at
`limit_ + 0xf`. None of it was a sizing problem, and none of it was arithmetic that
could be corrected by a better bound -- `mmap`, `munmap` and `mprotect` were *inferring*
ownership from `base_`, `brk_`, `limit_` and page alignment, and every inference was
wrong somewhere. `specs/clang-on-aegir.md`'s Phase 3 has all four, with the runs that
ended them.

So the question gets an answer instead of an inference. **`mmap` records every run it
hands out -- `(base, pages)` -- and `munmap` and `mprotect` accept nothing that is not in
that record.** A release of an unrecorded range is `EINVAL`, not a region returned to a
free list; a protection change on an unrecorded page is refused. The record's storage is
the self-hosting shape the heap's region list already uses (`grow_node_region`), so there
is no capacity to choose and no fixed ceiling.

That record is also the spine of *one* allocator rather than two. With a single ledger of
what is out, the arena, the streaming frames and the allocator's node pool stop being
three tenants inferring a boundary and become clients of one owner: `Scratch::reserve`'s
two ends -- the window's top for reservations, its base for streaming -- get a ledger to
reserve against, and any release can only accept what the ledger says was handed out.
This is the piece to build first; the rest of the merge is arithmetic that the ledger
makes checkable instead of arguable.

**Landed** (`libs/hosted/aegir-heap/src/heap.cc`): `mmap` records every run it issues, and
`munmap` releases only a recorded run, whole. `mprotect` deliberately keeps its arena
bound rather than asking the record -- the ledger says what `mmap` *issued*, and the
heap's pages are not all mmap runs. Asking the record there broke the C++ smoke's TCB
check, because a thread's clone needs a protection change on heap memory, and it took a
dozen runs to see for a reason worth keeping: `sys_mprotect` traced only *successes*, so
a refused protection change was invisible while the log looked clean. Both of its
refusals are traced now. Measured green: `CXX_SMOKE_OK` (and its `std::thread` check)
pass, zero refusals of either kind, `PASS aegir-8g-smp4 booted`, `RUN_EXIT=0`.

**And the growth the record was meant to enable is not landable yet -- now for a named
reason.** With `mmap` reserving its runs from the window's top (the arena itself a
reservation, the bounds and the frame record keyed to the window so those runs are
owned), the C++ smoke's thread check fails deterministically, and it says why:

    cxx-smoke: thread failed: thread constructor failed: Resource temporarily unavailable

That is `EAGAIN` out of `pthread_create`: the thread is never created. It rules out the
join, the TLS and the mutex in one line, and it points at the creation path -- the
stack's `mmap` and the guard's `mprotect`. The instrument that produced it is one line
in the *smoke*, not the heap: `catch (std::exception const &error)` and print
`error.what()`, where a bare `catch (...)` had been discarding the reason while this was
the one check failing. What is still silent is `map_page`'s own failure -- the frame it
asks the allocator for, which is how `mmap` would come back `ENOMEM` and `pthread_create`
turn into `EAGAIN` -- so the next attempt starts by tracing that, and by checking all
three refusal counts (`mmap-refused`, `munmap-refused`, `mprotect-refused`) rather than
the two that happened to be grepped.

**And the placement, not the cursors, is what breaks it: with every growth removed the
faults are byte-identical.** `reserve` and `map()` now both refuse instead of raising the
window's end, so nothing claims address space it does not own -- and the same five
services fault at the same addresses, each exactly one page past its own window
(`0x4012f00f` for a window ending `0x4012e000`, `0x400fe00f` for one ending `0x400fe000`),
with `cxxsmoke` dying *before* its thread check, which is why the probe never printed.
So the cause is neither cursor: it is the placement. Two candidates, and the next attempt
takes them in order -- `mmap`'s runs coming from the window's top instead of the arena's
`mmap_`, or the arena-as-reservation itself (`heap::init`) and what the services still
hold that it no longer covers.

**And removing the heap cap alone is not enough -- measured.** `heap::init(..., 0)` now
means "as much of the window as there is", and `command.h`'s `kHeapBytes` is gone, so a
hosted command's heap is its whole window. The run stops at the session's *first* command:
the five POSIX markers never fire and no `Sys:Development/C/cc` line appears, because the
arena took the window the **scratch** needs for its streaming frames. Same wall as the
first attempt, from the other side.

So the two changes belong together, and neither works alone: the cap has to go (a
program's size is not ours to choose) *and* the arena and the scratch have to stop being
two tenants of one window -- one allocator over one space, which is what the user asked
for and what this whole arc keeps arriving at. The measured constraint on that allocator
is now known exactly: **a process cannot map past its window** (`window-probe: REFUSED`,
every process, green run), so the one space must be *shared*, not *extended*.

## The arc: one space, one owner

Three fixed choices are stacked in the memory path, and each one has now been hit:

- **A process cannot map past its window** (above). Address space cannot be extended, only
  shared -- so every "grow the window" fix was doomed, and the probe is what says so.
- **The window's size is the spawner's number** -- `window_base`/`window_bytes` in the
  bootstrap block, read by `adopt_memory()`; the launcher writes what a nested program gets.
- **The heap's seed is a constant** -- `kHeapBytes`, 8 MiB for a hosted command, plus a
  hand-written number in every hosted app. `ld.lld` maps ~45 MiB to link.

Inside that one window live **two tenants**: the heap's arena at the top (placed by
`map_at`, per that method's own comment) and the scratch's streaming frames from the base
(`map`/`map_large`). Two cursors, neither able to see the other: giving the arena the whole
window starves the scratch (measured -- the session's first command fails), and the
scratch's `map()` growing the window faulted one page past it (also measured).

So the arc is one owner for the one space:

1. **`Scratch` hands out runs and takes them back.** `map`/`map_large` stream from the base
   up, `reserve` walks down from the top for long-lived runs, and `release` takes a run
   back when it ends where the reservation cursor is. No growth: the window is what the
   process was given, and its limit is fixed.
2. **One free list, kept by the process, not by the window.** The scratch owns addresses and
   no frames, so it cannot store a list -- an earlier attempt to make it did, and the faults
   are recorded above. The heap owns frames (its node storage grows on demand through
   `grow_node_region`) and already keeps one (`g_free_regions`). A released run is reused
   from that list first and reserved from the window only when the list is empty.
3. **Then the seed goes.** Once runs are allocated per need, `kHeapBytes` bounds nothing: a
   program's heap is what it asks for, and the only ceiling left is the window -- the
   spawner's number, and the last one to remove.

None of it needs a kernel, musl or LLVM change: frames are unbounded through `mem.main`'s
`kMethodAlloc`, page tables are created on demand, and the three choices above are the only
limits in the way.

### One space, one owner: what it took (measured)

The arc is landed, and it took four corrections, each of which the run named:

1. **Everything the heap places comes from `reserve`.** The free list's nodes, the run
   record and the frame chunks were carved *down from `mmap_`* -- and `mmap_` is the arena's
   *top* -- so the heap's own bookkeeping was laid on the arena's pages. The kernel said so
   exactly: `[decodeRISCVFrameInvocation/893 ... "font"]: Virtual address (0x4012d000)
   already mapped`, one page below each service's window end. Three sites, one source now.
   (`mmap_` keeps its other meaning: `brk`'s ceiling, the arena's end.)
2. **`set_rights` and `sys_mprotect` bound by the *window*, not the arena.** `mmap`'s runs sit
   *below* `base_` because the arena is reserved first, so an arena bound refused a
   legitimate `mprotect` -- silently, which is how `posix-memory-test` failed check 10 with
   the kernel saying nothing at all.
3. **`map_page` records every page it maps *inside the window*.** It recorded only arena
   pages, so a reserved run's frames were never in the record -- and `set_rights` needs
   exactly that to name the frame behind a page. This is the one the trace found:
   `mprotect-refused 0x3f898000 0x1000` for the client whose pages came from `mmap-reused`.
   (The cycle its comment warned about -- the record growing through arena pages -- is gone,
   because the registry's pages come from `reserve` too.)
4. **The streaming cursor stops at the reservation cursor** (`map`/`map_large`), and a
   service's window no longer grows: measured, `window-probe` is REFUSED for every process.

**Result, on `aegir-8g-smp4`:** no service faults at one page past its window -- the
signature that killed `font`, `fssmoke`, `envsmoke`, `cxxsmoke` and `hello` in every earlier
attempt -- and `CXX_SMOKE_OK` fires where `cxxsmoke` used to die before its thread check. All
five POSIX clients pass (`WAIT_OK`, `PATH_OK`, `FILE_OK`, `MEMORY_OK`, `ENV_OK`), zero
`mprotect-refused`, zero `already mapped`. And the session's script reaches the acceptance:
`launcher: command started Sys:Development/C/cc`, with the compiler then reserving region
after region (`region 0x4401d000 0x4481d000`, `mmap 0x4401c000`, `mmap 0x44013000`, ...) --
a 97 MiB program taking what it needs, past a seed that used to stop it.

What limits the run now is not a cap in the heap but the runner's own quiet timeout
(`FAIL no console line within 300s`): clang compiles silently for longer than that. That is
the next thing to decide -- a development target that runs a real compile needs a quiet
allowance that fits one, and the acceptance's remaining steps follow it.

## What this is not

- **A pager or swap.** The pool is RAM; there is no backing store.
- **A general frame broker.** The service still hands out pristine untyped; a
  *service* that needs frames asks the `FrameRegion` library for them, which is
  what keeps the retype sequence out of the service. A device's frames stay the
  device manager's (`specs/services.md`).
- **A complete policy engine.** One resource, two actions and three subjects
  are the first cut; per-service and per-session subjects, a growth a supervisor
  confirms (`confirm`), and `signal`/`throttle` are later (`specs/limits.md`).
