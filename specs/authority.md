# Identity, authority and accounts

Status: proposed, for review (2026-09).

Aegir is multiuser, which is not a property of the kernel: seL4 has no users, and
it should not. What Aegir has instead is a kernel-enforced answer to "who sent
this message" — the badge — and an Aegir-level record of what each process has
been given and what it may do. This file defines identity, the two classes of
authority, the right to spawn, and the accounts that make resource use
attributable.

`specs/director.md` describes the process that hands out authority;
`specs/services.md` describes the services that hold it.

## Identity is a badge

The kernel reports the sender of a message as "the badge of the endpoint
capability that was invoked by the sender"
(`kernel/libsel4/include/sel4/syscalls_master.h:37-43`). Badges are minted with
the capability, by whoever is installing it (`seL4_CNode_Mint(..., badge)`,
`out/aegir/libsel4/include/interfaces/sel4_client.h:2415`), and a sender cannot
change what its capability says.

So identity is not a claim in a message that a service has to believe; it is a
property of the capability the sender had to hold in order to speak at all. An
Aegir identity is that badge plus a human-readable name plus an account.

## The two classes of authority

| | system | user |
| --- | --- | --- |
| created by | director, at boot from the manifest, or by an elevation request | director, on request of an authenticated user |
| authority | exactly what its manifest entry declares: custody, ports, an account | its session's ports and volumes, and its account |
| device capabilities | per declaration, least-authority | not declarable (`specs/services.md`); a specific device may be granted at runtime by the device manager and recorded in the account — see Open, for review |
| `IRQControl` | custody is delegated to the device manager | never |
| may spawn | yes, within its account and its declared spawn right: a service that launches children (device manager → drivers, partition manager → filesystems) declares what it may start | yes: a session spawns user processes as ordinary use — a terminal, the desktop, a launcher — and everything it starts has at most the session's authority |
| how others see it | a system badge | a user badge — what the VFS and every other service checks |

The kernel enforces the floor of this model and nothing above it:

- a process can only hand on what it holds — `seL4_CNode_Copy` and
  `seL4_CNode_Mint` require the source capability
  (`out/aegir/libsel4/include/interfaces/sel4_client.h:2345`, `:2415`);
- a process can take back everything derived from a capability it owns —
  `seL4_CNode_Revoke` (`:2167`).

Authority along the spawn tree is therefore non-increasing **by construction**.
Aegir's policy is written on top of a floor it cannot violate, which is what makes
"director hands authority outward" and "no process exceeds its grant" the same
statement (`specs/director.md`).

## The right to spawn

Spawning is not a privilege held by one process. It is a capability — a pool to
build address spaces from, memory to build them with, and the authority to be
handed down — and Aegir hands it to the processes that need it, scoped to what
they are:

- **Director spawns the system.** Boot services are created by director as
  superuser, with the authority their manifest entry declares. Director keeps
  `ASIDControl`, which is what mints pools, so *system authority* has exactly one
  source.
- **Services spawn their own children**, inside their account and the spawn right
  the manifest declares for them: the device manager starts block drivers, the
  partition manager starts filesystems. This is the requirement from the design
  brief ("the device manager should launch block drivers") taken literally — the
  service that knows *what* should exist is the service that creates it.
- **Users spawn user processes.** A session is created with user authority, a pool
  and an account, and a terminal, desktop or launcher starts processes from it
  without asking anyone. Everything a session spawns has at most the session's
  authority, and is charged to the user's account.
- **Elevation is the only user→system path.** A sudo-like tool asks `auth`, which
  does the credential check, and director, which owns system authority, to run the
  requested program — or to start the named manifest entry — with system
  authority. What it grants, for how long, and whether it can be inherited are
  policy decisions recorded below; what it must not do is leak.

The kernel is what keeps this safe, and it does so without knowing what a spawn
*is*: a process can only hand on what it holds (`seL4_CNode_Copy`,
`seL4_CNode_Mint`), it can take back everything derived from a capability it owns
(`seL4_CNode_Revoke`), and an address space is built from a pool it either has or
does not. Every spawn is therefore charged to an account and bounded by the
spawner's authority, whoever the spawner is. There is one spawn *implementation*
— the same library, the same ABI, used by every spawner — and many spawners, which
is why a process can be traced to the spawner that created it rather than to a
central service that created everything.

Two consequences worth stating plainly. Authority is monotone down the spawn
tree: a session cannot start a system process, and a driver cannot start something
with more authority than the device manager allowed it. And a *policy* question
that used to be director's is now distributed: what a service may spawn is
declared in its manifest entry, and what a user may run is the session's business
(`specs/services.md`).

## Accounts and accounting

An **account** is the record of what a process, a user or a service has been
given, and everything a process holds is charged to exactly one.

What is recorded:

| Recorded | Why it is the currency |
| --- | --- |
| untyped memory held, by size class | the only thing the kernel allocates from; every object is retyped out of it (`seL4_Untyped_Retype`) |
| objects by type | derived, but it is what a person actually asks about |
| threads | a process's concurrency |
| CSpace slots | the other finite thing a service can exhaust on itself |
| ports owned and held | what it can be reached by, and what it can reach |
| device frames, IRQ caps | custody rather than usage: authority, recorded for attribution |
| volumes and files | later: charged through the VFS when it can report them |

**Capacity grows on demand.** This is the model, not a tuning detail: an account
has no hard ceiling to hit, because that would be an arbitrary limit baked into
the system (project rule: no arbitrary and hardcoded limits; capacity tables grow
on demand). A process that needs more asks; the request is granted from what the
machine has. What the manifest carries is a *starting* grant and a growth policy
— never a fixed maximum. The only real ceiling is machine memory, and the point of
keeping records is to make "who is holding it" answerable *before* that ceiling is
reached: growth without visibility is how a machine runs out of memory by
accident.

Growth applies to pools as much as to memory. A session that has started one
program too many has run out of *address spaces*, not of bytes, and asks for
another pool the same way it asks for another megabyte. That is what keeps "no
arbitrary limits" from meaning "no limits at all": the limit is the machine, the
request is visible, and the answer is a decision someone made rather than a
number someone guessed when the system was built.

Reclaim closes the loop without a bookkeeping daemon, and it is why an account is
a set of capabilities rather than a table. An account is backed by what director
*retains* when it grants: a copy of every untyped it handed over, and a capability
to the child's root CNode. `seL4_CNode_Revoke`
(`out/aegir/libsel4/include/interfaces/sel4_client.h:2167`) deletes the
capabilities derived from the capability it is invoked on, so revoking a retained
untyped cap is what frees the objects retyped from it — the kernel's own retype
rule assumes exactly that ("RevokeFirst: The untyped has been used to retype an
object. Or, a copy of the untyped capability exists", `:419`). Keeping a copy of
what it grants is therefore not bookkeeping: it is the reclaim path. Accounting
that needs the allocating process to be alive in order to be correct is accounting
that is wrong exactly when it matters.

The *policy* — when a growth request must be confirmed rather than granted, and by
whom — belongs to the system class and is declared in the manifest. Director is
the mechanism that applies it, not the authority that decides it.

### The honest gap: CPU

**CPU time is not accounted and cannot be enforced in this configuration.**
`KernelIsMCS` is off — `config_option(KernelIsMCS KERNEL_MCS "Use the MCS kernel
configuration, which is not verified." DEFAULT OFF)` (`kernel/config.cmake:7-8`) —
so threads have no scheduling context with a budget and a period.
`seL4_CapInitThreadSC` is a null cap in our bootinfo
(`kernel/libsel4/include/sel4/bootinfo_types.h:29`). The kernel's budget and
period checks exist and are real (`kernel/src/object/schedcontrol.c:97-135`), but
only when MCS is on.

What can be said today: memory and authority are accounted and enforceable; CPU is
not. The kernel's `track_utilisation` benchmark can *observe* per-thread
utilisation (`kernel/config.cmake:220-225`), and observation is not enforcement —
it is a debugging aid, not an accounting story.

Turning MCS on would make CPU a real currency and would cost the verified
configuration claim (`specs/build.md` deliberately trades that already, for the
default RISC-V ABI) plus a change to priority and scheduling semantics across
every service. It is an open decision, not a deferred default.

**Placement is a separate decision from budget.** With more than one core
(`specs/build.md`'s envelope), which core a service runs on is a resource
decision of its own: the kernel brings the other harts up and then leaves them
idle, and a thread runs where it was told to (`seL4_TCB_SetAffinity`). Director
currently gives everything to the core it started on, which is the honest default
while there is one service — and the manifest is the obvious place to declare
placement when there are more, because it is already where a service's authority
is declared. Not designed yet; recorded so the first multi-core service is not
the thing that discovers it.

## Multiuser

What multiuser means here, concretely:

- A **user** is an identity: a badge, a name, an account, and later a home volume.
  Users are records Aegir keeps, not a kernel concept.
- A **session** is the set of processes created for a user after authentication.
  Nothing about a session is special to the kernel; they are ordinary
  user-authority processes.
- **Ownership and permission** are enforced by whichever service owns the
  resource, using the caller's badge: the VFS for files and volumes, the device
  manager for devices, `auth` for identity changes. There is no kernel-level user
  to check against and none is needed — the badge is the check.
- **The system class is the superuser**, and it is not a user: it is the boot
  services, plus whatever elevation hands out temporarily.
- Because a user's authority is a set of capabilities rather than a user id,
  "user A cannot read user B's files" is the same mechanism as "user A has no
  device capability": the capability was never handed over. Access control here
  is not a check that can be skipped by a buggy caller — it is the absence of a
  capability.

## Open, for review

- **Credential storage and method.** Decided for the first slice
  (`specs/auth.md`): the database is a binary table packed at build time from a
  descriptor-row source, carried in the initrd and read through the namespace;
  v1 sends the plain secret over the port, honestly interim, and login becomes
  a challenge when user sessions give the port listeners. What stays open is
  the hash and the challenge exchange of that later arc.
- **What elevation grants.** Via the sudo-like tool: `auth` checks the credential,
  director performs the spawn. Proposal: a one-shot system process per request
  (running the named program, or starting the named manifest entry), charged to
  the requesting user's account, logged by `logger`, and not inheritable — so an
  elevated command cannot leave system authority behind in the session that ran
  it.
- **What a session is given at creation** — its pool, its account, its home
  volume, its ports — and what a session may run: whether starting a program needs
  a declaration at all, or whether anything on a volume the session can read is
  fair game. This is the interface `auth`, director and the user's environment
  meet at, and it is not yet specified.
- **Growth policy details.** The owner is the system class, declared in the
  manifest (see Accounts and accounting, above); what is open is only its shape —
  at what point a growth request must be confirmed instead of granted, and what
  happens at that point. Either way: no hard ceiling, and no fixed per-user quota.
- **May users hold device capabilities?** A session's framebuffer is the obvious
  case. If yes: granted per device by the device manager, recorded in the account,
  revoked with the session.
- **What a session contains** — shell, workbench, whatever comes next — is a later
  spec.
- **POSIX-style uid/gid compatibility is explicitly not part of this model.** A
  POSIX compatibility layer, if it is ever built, maps uids onto Aegir identities
  as an ordinary service on top (`specs/userland.md`).

## Spawn rights, in pieces

The requirement is that the *device manager* starts the drivers for the devices it
finds -- and today only director starts anything, because starting a process needs three
things director holds: address space ids (an ASID pool), memory (untyped), and the
binaries. So this becomes delegation, in pieces rather than in one leap.

**Done.** `Allocator::carve_untyped` cuts an untyped out of the machine's memory and
returns the *capability*, which is the shape the kernel wants for a pool: a pool is made
from an untyped (`seL4_RISCV_ASIDControl_MakePool`) rather than by retyping, so no
`alloc_object` path could have produced one. `Allocator::make_asid_pool` uses it and
keeps the architecture-specific call inside the library, so a caller does not have to
know which architecture it is on. Every boot says so:

    device manager pool: made, cap 256

**Done.** That pool, an untyped, the device manager's own VSpace root with a window of
free addresses, a copy of the initrd, and the device frames its children are for are all
delegated at boot (`44fea54`). The mechanism needed no invention: the bootstrap block
already carried `Capability` entries mapping a name to a slot, and granted capabilities
are installed exactly the way ports are (libs/aegir-spawn, `install`); what it grew was
the extra list of grants beyond the manifest's ports (`Binaries`, `Window` and
`DeviceCapability` block entries, format v3), because a pool is not a port. Every boot
says so:

    my own address space's root: cap 11, with a window of my own from 0x40000000, 1024 MiB of it
    the initrd: 716 KiB at 0x40200000, to start processes from
    a device to hand on: physical 0x10008000, 4096 bytes, frame at cap 12

**Done.** The device manager starts the block driver itself (`d7f1a8a`): it probes each
granted frame through its window for the virtio id in the device's registers, carves the
driver's queue memory out of its untyped, mints the driver a log port badged with who
the driver is, spawns it, and waits for the driver's ready before signalling its own:

    spawned blkdriver for virtio device 2, badge 256
    read sector 0: status 0 (0 is ok), 513 bytes used

The remaining pieces were not the ones director already assembles -- they were four
kernel rules the spawn path had never needed before, each recorded below.

### A retype's destination, and why the depth is the whole story

The device manager holds a pool and an untyped and uses both -- it retypes a root page
table and takes an address space id from its own pool, which is the first thing in Aegir
a service does with delegated authority:

    my own address space: page table at cap 11, with an address space id of my own

The one thing that stood in the way is worth writing down, because it was *read* rather
than guessed at, and because it is the shape of every retype that puts caps in a
service. `decodeUntypedInvocation` (kernel/src/object/untyped.c):

    if (nodeDepth == 0) {
        nodeCap = rootSlot->cap;                      /* the destination IS root */
    } else {
        lu_ret = lookupTargetSlot(rootCap, nodeIndex, nodeDepth);
        if (lu_ret.status != EXCEPTION_NONE) {
            userError("Untyped Retype: Invalid destination address.");

So **`node_depth == 0` means "the destination CNode is the capability I passed as
`root`"**, and `node_offset` is the slot inside it. A non-zero depth means something
different: *look the destination up* inside that CNode, where the depth has to agree with
its guard -- which is why director's `seL4_WordBits` idiom works for director's own CNode
(the kernel's, 8192 slots, a 51-bit guard) and came back "Invalid destination address" for
a service's (1024 slots, a 54-bit guard). The rule for a service retyping into its own
CSpace is `depth = 0` and the CNode itself as the root.

### A delegated untyped has to be told, not asked

A service cannot ask the kernel how large an untyped is -- there is no invocation that
reads one -- so a delegated untyped has to come with its size, or the service that holds
it cannot use it for anything. The size travels in the bootstrap block, on the
`Capability` entry for the memory (`Entry::reserved`, which nothing else uses), and
`spawn::PortGrant` carries it from director to the block:

    my own memory: 12 bits of untyped, as the block says

Zero for everything else, which is every port: a port has no size. The field is the last
member of `PortGrant` on purpose, so the places that build ports by aggregate
initialization are unchanged -- and it is *last* rather than merely appended anywhere,
which a `-Werror=missing-field-initializers` warning pointed out the hard way when it
landed between `rights` and `badge` and quietly turned every port's badge into a size.

### A untyped that has been split cannot be given away, and why that matters

The delegation is a page of untyped, and the next thing it needs to cover is a *process*:
a CSpace with the slots a child is given is 16 KiB on its own. Delegating 32 KiB instead
failed, and it failed in the most instructive way -- the pool was made, the allocator
reported `largest 2^29` free, and then:

    FAIL a port could not be installed into the child

The capability was not null; the *kernel* refused to install it. The difference between
the page that works and the 32 KiB that does not is **splitting**: `Allocator::carve_untyped`
splits a larger untyped down to the size asked for, and each split *derives an object from
the parent*. seL4 will not let a capability with derived objects be copied or moved until
they are gone -- the manual's error table says it in as many words:

    Revoke First: The object currently has other objects derived from it and the requested
    invocation cannot be performed until either these objects are deleted or the revoke
    invocation is performed on the capability.

A page can come from a untyped that is exactly a page (no splitting, no children). 32 KiB
cannot, on this machine, without splitting first.

**Resolved: hand over the leaf.** `Allocator::carve_untyped` splits down to the size
asked for and returns the *leaf* the split produced (`5a9f323`), which has nothing
derived from it and so may be installed into another CSpace. The revoke hammer was
never needed.

### A mint's source is addressed the caller's way, not the callee's

The first service-side spawn died installing the child's own TCB with `FailedLookup`,
and the lookup fault said why: `DepthMismatch, 54 bits unconsumed`. The root task's
initial CNode cap carries a guard of `seL4_WordBits - radixBits`, so a plain slot number
resolves at full word depth; a service's own-CNode cap (slot 2, `kSlotOwnCNode`) is a
*raw copy* with guard 0 and radix `kCNodeBits`, and with no guard the index is read
MSB-first (`kernel/src/kernel/cspace.c:126-193`) -- so a plain slot at depth 64 names
slot 0 and leaves 54 bits nobody reads. `seL4_CapInitThreadCNode` is, on top of that,
the *root task's* name for its CSpace, not anyone else's.

The kernel offers no invocation to ask which of these the caller is, so the caller says:
`Spawner` takes the source root and depth it addresses mint sources through
(`seL4_CapInitThreadCNode` at `seL4_WordBits` for director, `kSlotOwnCNode` at
`kCNodeBits` for a service), and `kCNodeBits` moved to `aegir/bootstrap.h` because the
guard a child's TCB is configured with and the depth its spawner addresses it at are the
same constant.

### A fresh TCB may confer no priority at all

`create_object` for a TCB sets only the fields that differ from zero
(`kernel/src/object/objecttype.c:525`), so a new thread's maximum controlled priority is
**0**, and `SetPriority` refuses a priority above what the authority cap's MCP allows
(`kernel/src/object/tcb.c:1236`). Director never noticed -- its own MCP is the maximum.
A service spawning its first child did: the spawner now raises the child's MCP
(`SetMCPriority`, within what the spawner's own TCB may confer) before setting its
priority.

### A badged endpoint cap cannot be minted again

Two installs failed with `seL4_IllegalOperation` and the kernel's own words, "Mutated
cap would be invalid": the device manager's own `log.main` cap is badged with who *it*
is, and so is the fault endpoint it was given. Deriving a differently-badged cap from an
already-badged endpoint is exactly what the kernel refuses. The shape that respects it:

- **a spawning service is given an *unbadged* copy of every port its children need to
  call**, under a `spawn:`-prefixed name in its block, so the badge it mints for a child
  is the first badge that cap ever carries (director builds the list from the spawned
  entry's port grants);
- **its children's fault endpoint is one it makes**, not the one it was given -- a
  spawning service supervises what it spawns, and its own badged cap could no more be
  re-badged than the log port's.

The delegatable copy travels *down* the chain the same way it arrives: the device
manager grants the partition manager its own `spawn:log.main`, unbadged, because the
partition manager's `log.main` is badged with who *it* is and cannot be re-minted for
the filesystem services it starts either.

The same rule shapes how a volume capability reaches a client. A filesystem
registers its volume port's caller half *unbadged* (the partition manager keeps the
half it minted at the spawn; the initrd service mints one itself), and the VFS mints each
resolver's own badge onto the copy a `resolve` hands out -- so the filesystem learns
who is asking from the kernel, through a hand-off in the middle, and the identity
chain is never broken by the map that made it (`specs/vfs.md`). A range or a
permission that one day clamps by badge has the true caller to clamp by.
### A frame cap serves one address space; the copies come first

A frame's *first* mapping pins it: the mapped ASID and address are written into the
**capability** at map time, and a later map of the same cap into another VSpace is
refused with `seL4_InvalidCapability` ("Attempting to remap a frame that does not
belong to the passed address space", `kernel/src/arch/riscv/kernel/vspace.c:869-878`;
the same check on ARM, `kernel/src/arch/arm/64/kernel/vspace.c:1568`). But the pin is
in *that cap*, not in the frame: a copy made **before** any mapping carries no ASID
and may be mapped into a different address space -- which is how one physical page
serves two VSpaces, and the only way.

The storage stack lives on this. A block port's shared window is mapped into the
driver *and* into every client, so the device manager mints copy sets of the window's
frame caps before anything maps them: the originals go to the driver, one set stays
pristine as the source for client grants, and each consumer's set is minted from a set
nobody has mapped. A copy made *after* a mapping inherits the mapping's ASID and is
useless -- the order is the whole mechanism. The partition manager is granted two sets
per port -- its own, and one reserved for the children it starts -- so a set it never
maps can mint mappable windows for filesystem services without bound.

### IRQControl cannot be copied; custody moves whole

`deriveCap` on `cap_irq_control_cap` succeeds and produces a *null* cap
(`kernel/src/object/objecttype.c:75-78`): a copy of IRQControl is nothing, and
installing one fails the mint's own check ("Mutated cap would be invalid",
`kernel/src/object/cnode.c:176-179`). So the kernel's one well of handler caps cannot
be delegated the way the untyped and the ASID pool were. What crosses is custody:
`seL4_CNode_Move` takes the cap as it stands (`kernel/src/object/cnode.c:155-161`),
the device manager holds it outright, and the root task's slot is empty from then on.
The spawner's `PortGrant` grew a `move` flag for exactly this; nothing else moves.

That placement is the right shape for the kernel's other IRQ rule: one handler cap
per IRQ, a second `seL4_IRQControl_Get` for the same number being
`seL4_RevokeFirst`. The issuer is the service that knows which binding has which
IRQ. What a driver receives is the pair, made at the binding and armed before the
child starts -- the first `Ack` is what lets signals in
(`projects/sel4test/apps/sel4test-driver/src/main.c:555-582`): a notification it
waits on after each kick (Read is the whole grant), and the handler it acks after
each signal. On RISC-V the Ack's work was already done by the claim in
`getActiveIRQ` (`kernel/src/object/interrupt.c:136-143`), but the sequence is the
portable one, and a driver that finds no pair polls.

### The sizes, measured

- The delegation to the device manager is **2 MiB** (`kDelegatedUntypedBits = 21`):
  the driver's image (~172 KiB of frames, its queues are static storage), its 8 KiB
  of virtqueue, the 64 KiB shared window, the partition manager's image and the
  1 MiB it is delegated, and the filesystem service's image handed over as bytes.
  256 KiB, 512 KiB and 1 MiB were each measured too small rather than guessed.
- The partition manager's own untyped is **1 MiB**: each filesystem child costs
  its image copy (~180 KiB), its objects, and a 64 KiB window set of its own, so
  two partitions already ask for most of a megabyte.
- The whole initrd is **not** delegated: 1.2 MiB does not fit a service-sized
  delegation, so a service that starts one known helper is handed that helper's image
  as a blob, and `spawn::Request.binary_image` reads bytes instead of an archive. The
  day a service starts helpers it cannot name in advance, the answer is a narrower
  initrd, not a bigger delegation.
- Badges for a service's spawned children count from **256** for the device manager's
  (the low badges are director's boot set) and from **512** for the partition
  manager's -- until the badge space is a designed thing, each spawning service's
  children live in a range of their own.
