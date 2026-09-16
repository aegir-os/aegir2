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

- **Credential storage and method.** Where the user database lives (proposal: an
  initial copy in the initrd, authoritative copy on the root volume, `auth` waits
  for the volume — see `specs/services.md`), what it stores (which hash?), and
  whether login is a challenge over a port rather than a password crossing it.
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

**Next.** Give that pool, and an untyped, to the device manager. The mechanism needs no
invention: the bootstrap block already carries `Capability` entries mapping a name to a
slot, and granted capabilities are installed exactly the way ports are
(libs/aegir-spawn, `install`). What is missing is the extra list of grants beyond the
manifest's ports -- the pool is not a port, and handing it to the device manager is the
same act of delegation (specs/services.md).

**Then.** The device manager can retype a page table, assign it an ASID from its own
pool, and hold an address space of its own. That is the point at which "the device
manager launches a driver" becomes a statement about the device manager rather than
about director, and the remaining pieces -- the child's CSpace, its TCB, and the
binaries from the initrd -- are the same ones director already assembles.

### The destination of a retype, open

The device manager now holds a pool and an untyped and tries to use them:

    [seL4(CPU 0) [decodeUntypedInvocation/119 ... "devicemgr"]:
        Untyped Retype: Invalid destination address.]

So the *destination* of the retype is wrong, and that is the one thing to settle
before this works. Director's own retypes pass `seL4_CapInitThreadCNode` for both the
`root` and the `node_index` argument and `seL4_WordBits` for the depth, and land in the
slots they ask for -- but director's root CNode is the kernel's (8192 slots, a 51-bit
guard) while a spawned service's is 1024 slots with a 54-bit guard, and the number of
guard bits is exactly what a depth has to agree with. The next move is the source, not a
guess: `kernel/src/object/untyped.c`, the line the kernel named, and what it does with
`node_index`, `node_depth` and `node_offset` when a guard is in the way.
