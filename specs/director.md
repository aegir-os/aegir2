# The director: Aegir's root task

Status: proposed, for review (2026-09).

`director` is the first Aegir process: the root task the kernel starts, and the
only process that begins with the machine's authority. It is an init in the
strict sense — it creates the first services and gives each one exactly what
its manifest entry says — and it invents no policy. What it applies is the
manifest's (a service's restart rule) or the machine's (what memory is left),
never its own opinion. Every question it could answer on someone else's behalf is
answered by a service instead: a root task that grows policy becomes a root task
that has to be restarted to change policy.

`specs/services.md` defines the services it starts. `specs/authority.md` defines
who may do what, and who pays for it.

## What the kernel gives the root task

Verified in the pinned tree, not assumed: the initial capability slots are
enumerated in `kernel/libsel4/include/sel4/bootinfo_types.h:14-32`.

| Slot | Capability | What director does with it |
| --- | --- | --- |
| 0 | `seL4_CapNull` | empty, and left empty |
| 1 | `seL4_CapInitThreadTCB` | its own thread |
| 2 | `seL4_CapInitThreadCNode` | its CSpace root — the source of every capability it hands out |
| 3 | `seL4_CapInitThreadVSpace` | its address space |
| 4 | `seL4_CapIRQControl` | mints IRQ handler caps; a copy is delegated to the device manager (`seL4_CNode_Copy` leaves the original in place, so custody can be re-delegated after a restart) |
| 5 | `seL4_CapASIDControl` | makes ASID pools — and is **kept**: pools are minted here and delegated, which is how "who may spawn" is answered by a capability rather than by a central checkpoint |
| 6 | `seL4_CapInitThreadASIDPool` | its own pool; children get an assigned VSpace root, never a pool |
| 7 | `seL4_CapIOPortControl` | x86-only: null on RISC-V |
| 8 | `seL4_CapIOSpace` | IOMMU-only: null here |
| 9 | `seL4_CapBootInfoFrame` | the frame the bootinfo is in — how it reads what the kernel passed it |
| 10 | `seL4_CapInitThreadIPCBuffer` | its IPC buffer |
| 11 | `seL4_CapDomain` | the domain scheduling cap |
| 12, 13, 15 | `seL4_CapSMMUSIDControl`, `seL4_CapSMMUCBControl`, `seL4_CapSMC` | null caps in this configuration |
| 14 | `seL4_CapInitThreadSC` | a null cap: this kernel is not MCS (see `specs/authority.md`) |

Beyond the slots, what the kernel gives it is memory and data: every byte of RAM
it was given, enumerated in the bootinfo as untyped capabilities, and the empty
CSpace slots it may fill. The device tree arrives as one of those data items — an
extended bootinfo chunk (`SEL4_BOOTINFO_HEADER_FDT`,
`kernel/libsel4/include/sel4/bootinfo_types.h:108`), which on RISC-V the kernel
copies out of the DTB it was booted with
(`kernel/src/arch/riscv/kernel/boot.c:267`). Neither director nor the device
manager has to go looking through a private archive to find it.

## The invariant: no process exceeds its grant

Director begins with the machine's authority and only ever hands it outward. It
keeps what the spawn path needs — its own memory, the CSpace root, the ability
to build a process — and passes the rest on: `IRQControl` to the device manager,
untyped memory to accounts, ports to services.

Authority is monotone along the spawn tree, and the kernel is what keeps it that
way: a process can only pass on what it holds (`seL4_CNode_Copy` and
`seL4_CNode_Mint` need the source capability) and can take back everything derived
from a capability it owns (`seL4_CNode_Revoke`). Nothing downstream can create
authority director did not give. A grant can be *revoked* — that is the reclaim
path in `specs/authority.md`, and it returns memory to the pool — but it can never
be amplified. That is why "director hands authority outward" and "no process
exceeds what it was granted" are the same statement, and both are enforced below
us.

Concretely, that is how spawning is distributed without becoming unsafe. Making
an address space takes an ASID pool
(`seL4_RISCV_ASIDControl_MakePool`,
`out/aegir/libsel4/include/interfaces/sel4_client.h:423`), and a pool is what
hands out ASIDs (`seL4_RISCV_ASIDPool_Assign`, `:483`). Director keeps
`ASIDControl` — the authority to *mint* pools — and delegates **pools**, not the
controller: a spawner holding a pool can create address spaces, and one holding
none cannot. So "who may spawn" is answered by a capability rather than by a
central checkpoint, and the number of pools an account may hold is one more thing
that grows on demand (`specs/authority.md`).

## Responsibilities

1. Bootstrap. Build its own allocator, CSpace and VSpace from the bootinfo.
2. Read its initrd's boot manifest and validate it (`specs/services.md`).
3. Create each boot service: objects charged to that service's account, its
   capabilities installed — including the pool and spawn right its entry declares
   — its bootstrap block written, its thread started.
4. Hand over the custody that is not the spawner's business: `IRQControl`, device
   frames, the DTB copy.
5. Supervise: hold every child's fault endpoint, log what happens, apply the
   manifest's restart policy.
6. Keep the accounts and serve the growth requests that make capacity grow on
   demand (`specs/authority.md`).
7. Say when boot is complete — each service reported, then the boot marker.

## What director is not

| Question | Answered by |
| --- | --- |
| Which driver owns this device? | device-manager |
| Where is a file? | vfs |
| May this user do this? | auth, checked against the caller's badge |
| How does this partition get mounted? | partition-manager |
| What is between two colons in a path? | vfs |
| Where does this message go? | logger — and before it exists, the raw debug console |

## The spawn path is ours

Aegir does not spawn processes with `libsel4utils`. `specs/userland.md` already
fixed that position: the library is "a useful starting point and a reference for
capability bookkeeping, not Aegir's API". Owning the path costs us the work
below; it buys a CSpace layout, a startup ABI and an allocation policy that are
Aegir's, with no upstream shape to unwind later.

**One implementation, more than one spawner.** Spawning is a capability that can
be held rather than a privilege concentrated in the root task. One thing stays
concentrated: *system* authority, which has a single source — director. Beyond
that, most processes in Aegir come from someone other than director:

- **Director** creates the boot services, as superuser, at startup.
- **Services that launch children do it themselves**, inside their account and
  the spawn right their manifest entry declares — the device manager starts block
  drivers, the partition manager starts filesystems (`specs/services.md`). A
  device-discovery service that had to ask permission for every spawn could not
  react to a device appearing, and it would put the ownership of a driver in the
  wrong process.
- **Sessions start user processes.** A terminal runs commands, the desktop
  launches programs, a launcher runs what you pick: this is ordinary use of a
  multiuser system, it happens constantly, and it must not be a round trip to a
  privileged process. A session holds its own pool and its own account, and
  everything it starts has at most the session's authority.
- **Elevation is the exception**, and the only path from user to system: a
  sudo-like tool asks `auth` for the credential check and director — which owns
  system authority, and is the only spawner that can create a system process — to
  run the requested program, or start the named manifest entry, with system
  authority (`specs/authority.md`).

Capability invocations below are from
`out/aegir/libsel4/include/interfaces/sel4_client.h` (line numbers given so they
can be checked against a build), unless another path is shown.

| Step | Mechanism |
| --- | --- |
| Resolve the manifest entry to an initrd name | the manifest (data) |
| Read the ELF out of the initrd | `cpio_get_file` (`projects/util_libs/libcpio/src/cpio.c:200`) |
| Allocate the child's objects from its account's untypeds | `seL4_Untyped_Retype` (`:637`) |
| Build the child's CSpace, install its caps | `seL4_CNode_Copy`, `seL4_CNode_Mint` (`:2345`, `:2415`) |
| Build the child's VSpace and map its segments | `seL4_RISCV_PageTable_Map`, `seL4_RISCV_Page_Map` (`:119`, `:244`), frames charged to the account |
| Give it an IPC buffer and its program headers | auxv (below) |
| Configure the thread | `seL4_TCB_Configure`, `seL4_TCB_SetPriority`, `seL4_TCB_WriteRegisters`, `seL4_TCB_Resume` (`:893`, `:1022`, `:764`, `:1564`) |
| Charge the account | director's own records |
| Own the outcome | the child's fault endpoint, held by director |

Objects come from `seL4_TCBObject`, `seL4_EndpointObject`, `seL4_NotificationObject`
and `seL4_CapTableObject` (`kernel/libsel4/include/sel4/objecttype.h:10-13`), plus
frames — all of it retyped from untyped memory the account holds.

One thing the spawner deliberately does *not* touch: the child's floating point.
A fresh TCB has FP enabled — only the idle thread opts out — so a service can
compute in floating point because the kernel switches FP state per thread, not
because the spawn asks for it (`specs/build.md` records the three places that
decide it, and `apps/aegir-hello` checks it at boot).

### The startup ABI

A spawned process is an ordinary linked program (musl plus `sel4runtime`), so it
starts the way `sel4runtime` expects: a System V initial stack carrying
`argc`/`argv`/`envp`/`auxv` (`projects/sel4runtime/include/sel4runtime/start.h:19-25`;
the same contract `sel4utils_spawn_process_v` documents,
`projects/seL4_libs/libsel4utils/include/sel4utils/process.h:124-145`). What the
runtime actually reads out of that auxv is small and verified
(`projects/sel4runtime/src/env.c:282-320`):

- `AT_PHDR` / `AT_PHNUM` / `AT_PHENT` — program headers, which is where TLS comes
  from (`projects/sel4runtime/src/env.c:322`);
- `AT_SEL4_IPC_BUFFER_PTR` (67) and `AT_SEL4_TCB` (69) — the IPC buffer address
  and the child's own TCB slot.

The remaining seL4 tags — `AT_SEL4_CSPACE_DESCRIPTOR` (65),
`AT_SEL4_IPC_BUFFER` (68), `AT_SEL4_CNODE` (70), `AT_SEL4_VSPACE` (71),
`AT_SEL4_ASID_POOL` (72) — are declared in
`projects/sel4runtime/include/sel4runtime/auxv.h:20-29` and unused by this
version of the runtime. That unused space is the mechanism: **a child's
capability slots travel in the auxv**, addressed by the process on the other end
rather than by a table the spawner keeps.

Unknown auxv tags are ignored by the runtime
(`projects/sel4runtime/src/env.c:316-317`), so Aegir adds its own tag for the
bootstrap block instead of patching `sel4runtime`.

### The Aegir bootstrap block

One frame, mapped read-only into the child, located by an Aegir auxv tag. It is
versioned and counted so it can grow without becoming a new ABI:

- the child's identity — its service name, and the badge to expect on its own port;
- its account — what it is charged to, and how it may grow;
- its slots — for each declared port: the slot it was installed in, the port's
  name and protocol;
- its grants — the things that are not ports: device frames, a DTB copy, an IRQ cap;
- its supervision — the fault-endpoint slot and the restart policy applied to it;
- a version and an entry count, so an older reader can skip what it does not know.

The field list is fixed here; the layout is fixed with the manifest that produces
it in `specs/services.md`.

### The CSpace layout is data

Slots 0-2 are fixed by Aegir: 0 is left null, 1 is the child's own CNode, 2 is
its own TCB. From 3 upward, the manifest's declarations decide, in order. A
service's capability layout is therefore a function of its manifest entry —
readable, diffable and reviewable — rather than a convention buried in the
spawner.

## Supervision

Supervision follows the spawn tree: whoever creates a process installs the fault
endpoint for it, so whoever created it is who hears about it failing.
`seL4_TCB_Configure` takes that endpoint — in the variant this configuration
compiles (`out/aegir/libsel4/include/interfaces/sel4_client.h:893`, inside
`#if !defined(CONFIG_KERNEL_MCS)`; the MCS variant at `:962` has no such
parameter and is not the one built here) — and the endpoint must be "in the
CSpace of the thread being configured" (`:876`), so the sequence is: install a
copy of the endpoint into the child's CSpace, then name that slot when
configuring the thread.

A fault then arrives as a message whose badge identifies the child that faulted
(`specs/authority.md`). The spawner logs it and applies the policy for that child:
restart with backoff, stop, or stop together with the children *it* created. So
director supervises the boot services, the device manager supervises its drivers,
and a session supervises what the user started. A service's death is reported to
the process that made it — which is the only process that can make it again, and
is why supervision is not a separate service.

Two decisions make that work without a thread per child:

- **A fault is badged with the offender**, because the message carries the badge
  of the fault-endpoint capability the *faulting* thread holds
  (`kernel/src/kernel/faulthandler.c:41`, `:92`). So director mints a distinct
  badge per child — its service id — into the child's `kSlotFaultEndpoint`
  (`libs/aegir-bootstrap/include/aegir/bootstrap.h`), and every child can share
  *one* endpoint. Without a badge per child, identity would have to come from
  having one endpoint per child, and a supervisor would need a thread for each.
- **Director therefore runs two threads**: the boot thread, which waits for each
  service to report ready, and a supervisor thread blocked in `seL4_Recv` on the
  shared fault endpoint. seL4 cannot wait on two capabilities at once, and a
  second thread is cheaper than a select that does not exist. The supervisor logs
  the fault against the offending service and applies the manifest's restart
  policy; the boot thread's readiness wait is untouched by it.

Two boundaries are recorded rather than solved:

- **Nothing supervises director.** If it dies, no service can be restarted and
  there is no in-band recovery path. This is deliberate for now: a watchdog would
  have to be created by someone, which is the same problem one level up. Treating
  director's failure as fatal is the honest default until there is a reason to do
  something cleverer.
- **The raw console is a debug-build facility.** Before the logger exists,
  director and early services speak through `seL4_DebugPutChar`
  (`libs/aegir-runtime/src/debug.cc`), which only exists when the kernel has
  printing enabled — true in this configuration, false in a release build
  (`settings.cmake`, `specs/build.md`). A release build therefore *depends* on the
  logger being up. The logger is not optional furniture.

## Boot report

Director prints each service as it starts and, once every manifest entry has
reported ready, prints the boot-complete marker that `scripts/run_target.py`
watches for. Today that marker is `AEGIR_BOOT_OK`, printed by `apps/aegir-hello`
(`apps/aegir-hello/src/main.cc:124`); the code milestone moves it here.

"Ready" rather than "started" is the load-bearing word, and it is why the
manifest cannot be the whole story: a service that is running but has not yet
found its volumes has not finished booting (`specs/services.md`).

## Where `aegir-hello` goes

`apps/aegir-hello` becomes the first *spawned* client rather than a second root
task. It already proves the C++ path, the lp64d ABI, static constructors and TLS
at boot, and it needs no port at all, which makes it the cheapest possible spawn
smoke test — the thing to spawn on the way to spawning anything real. Keeping it
also keeps the M4 evidence alive in the tree.

## Open, for review

- Director's own failure is unsupervised, as above. Accept that, or design the
  watchdog now?
- Director is the only spawner of *system* processes, which makes its spawn port
  the single entry point for elevation and for creating a session. Should that be
  a service of its own rather than a port on director? The case for leaving it
  here: nothing else may hold system authority, so splitting it means inventing a
  second thing to trust.
- What a session is given when it is created — its pool, its account, its home
  volume, the ports it starts with — is the interface between director, `auth`
  and the user's environment, and it belongs in a spec before it is in code
  (`specs/authority.md`).
- Bootstrap-block delivery by auxv tag assumes a reader in the child. That reader
  belongs in `libs/aegir-runtime` (or a sibling `libs/aegir-bootstrap`) — the
  split is an implementation decision, not fixed here.
- Director's own capacity: it holds every untyped at boot. How much does it keep
  for itself, and when is the rest handed out? Proposal: everything not needed by
  a manifest entry is custodied by director until an account asks, so that
  "free memory" and "unassigned memory" are the same thing.


### Where supervision stands

Supervision works, and the boot now exercises it on every run. `aegir-hello` dies
on purpose before reporting ready -- it is the one service in the boot set whose
job is to fail, because a supervision path nobody walks is not a path -- and the
transcript shows the whole chain:

    supervisor: service 2 (hello) faulted on a memory access
    hello did not report ready: it faulted
    1 ready, 1 faulted
    AEGIR_BOOT_OK

That is the design working end to end. The kernel delivered the fault to the one
shared endpoint carrying the *offender's* badge (2, from the capability the child
was given, `kernel/src/kernel/faulthandler.c:83-96`); the supervisor was waiting on
it there, named the service from the table, and signalled the dead service's own
readiness notification -- which is how the boot thread tells a "ready" (the
service's own badge) from a "died" (director's badge, which no service can produce
for itself). The boot then finishes with one service faulted instead of hanging.

Two things had to be right for that to be observable, and both looked like
something else while they were wrong:

- **The root task must block when boot is done, not spin.** It runs at
  `seL4_MaxPrio`, so a spinning boot thread starves every service below it,
  including the supervisor that is supposed to report faults. Director now blocks
  on a notification of its own -- which is also where restarts and the elevation
  path will arrive. And note that `make run` stops the machine at `AEGIR_BOOT_OK`,
  so nothing after the last readiness wait is observable in a run: the case that
  *is* observable is a service dying before it reports ready, which is the case
  supervision exists for, since the boot thread is still waiting then.
- **A thread started by hand needs `gp` and `tp` in its user context**
  (specs/userland.md). With them zero the supervisor received the fault, faulted
  itself at address zero on its first IPC-buffer access, and blocked on its own
  fault -- indistinguishable from "the fault never arrived".

The whole delivery chain was checked by reading before the cause was found, which
is worth recording because it ruled out the right things: the spawner installs the
fault endpoint into the child's CSpace *before* `seL4_TCB_Configure`, with the
rights the kernel requires and the child's badge
(libs/aegir-spawn/src/process.cc:283-315); `Configure` gets the child's CSpace guard
and the CPtr `3`, and its error is checked
(libs/aegir-spawn/src/process.cc:349); the director hands the children the same
endpoint the supervisor receives on
(apps/aegir-director/src/services.cc:52,102); and the child's capability is a mint
of it, so both sides name one endpoint object.

**Open, in the order worth doing them:**

- The manifest's `restart` policy is not applied: `restart = always` on the logger
  means nothing yet, and the supervisor reports and suspends rather than restarting.
- Fault detail stops at "a memory access": the address and access type are in the
  fault message and are not decoded.
- The supervisor speaks on the console. Director has no port of its own yet, and
  the fault report is deliberately *not* an event in the logger's protocol: any
  holder of a port could announce someone else's death and the logger could not
  tell (specs/services.md).
