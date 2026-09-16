# Services, ports and the boot manifest

Status: proposed, for review (2026-09).

A service is a process with a name, a job, a set of ports and an authority class
— and all four are *declared*, not built in. This file defines the manifest that
declares them, the flat initrd their images come from, the ports they use to find
each other, and the boot set that brings Aegir up.

`specs/director.md` defines the process that creates them; `specs/authority.md`
defines what they may do and what they are charged.

## The initrd is a flat filesystem

The files director starts from live in its own CPIO archive, and that archive has
no directories. This is not a limitation being worked around; it is what the
mechanism already does:

- `MakeCPIO` archives each input by its **basename**, staging the file first so no
  source path can leak into the archive
  (`tools/seL4/cmake-tool/helpers/cpio.cmake:60`).
- Director finds an entry by exact name, first match, `NULL` if absent —
  `cpio_get_file` (`projects/util_libs/libcpio/src/cpio.c:200-222`).
- The archive can also be **listed**: `cpio_info` gives the entry count and the
  longest name, and `cpio_get_entry` returns entry *n* with its name (not
  NUL-terminated) and size (`projects/util_libs/libcpio/include/cpio/cpio.h:44-52`,
  `:64-70`). That is precisely a directory with no subdirectories.

So the initrd will mount as `Initrd:` — a volume whose contents are the flat
names, following the Amiga convention of a volume name ending in a colon. The
full path grammar is the VFS's business (its spec is not written yet); what this
spec fixes is that **the initrd has no paths**, and that its entry names are the
service identities.

Two archives exist and only one of them is ours. The ELF loader has its own,
holding `kernel.elf`, `kernel.dtb` and the stripped `rootserver`
(`tools/seL4/elfloader-tool/CMakeLists.txt:252-302`); that one is the loader's
private payload and no process can read it. Aegir's initrd is the archive linked
into director and reached through `_cpio_archive`/`_cpio_archive_end` — the
mechanism `sel4test-driver` uses for its own test ELF
(`projects/sel4test/apps/sel4test-driver/CMakeLists.txt:83`,
`projects/sel4test/apps/sel4test-driver/src/main.c:410-411`).

### Unique names are a requirement, not a convention

`cpio_get_file` returns the **first** match (`projects/util_libs/libcpio/src/cpio.c:200-222`),
so two entries sharing a basename shadow silently: a service would start from the
wrong binary and nothing would report it. Names in the initrd are therefore
required to be unique, and the image build must check it — the collapse happens
at build time, which is where the check belongs. Adding that check is a
code-milestone item, recorded here so it is not forgotten.

The initial contents are the service binaries plus the manifest. Both are flat
names; neither is a path.

## The boot manifest

The manifest is Aegir's service composition: what exists, what it may do, what it
is charged to, and what it needs before it can start. It is built into director's
image now, and will be served by a config service later, so that the service set
can change without reimaging. One format, three roles — source of truth in the
repo, boot input for director, and the config service's payload.

### Shape

A section per service, `key = value`, comments from `#` to end of line. It ships
as one file — `manifests/services.manifest`, packed into the initrd under its
basename — with the fields below; whether a large boot set should become one file
per service is the last item of this spec. It is deliberately dull: director
parses it from a memory buffer, with no filesystem and no dependencies.

```ini
format = 1

[logger]
binary    = logger
authority = system
account   = system
owns      = log.main
restart   = always
priority  = 200

[device-manager]
binary    = device-manager
authority = system
account   = system
owns      = devmgr.registry
needs     = log.main
grants    = irq-control, dtb
spawns    = blk.*
restart   = always
```

| Key | Required | Meaning |
| --- | --- | --- |
| `binary` | yes | the initrd entry to load — a flat name, and it must exist |
| `authority` | yes | `system` or `user` (`specs/authority.md`) |
| `account` | yes | the account it is charged to |
| `owns` | no | ports it owns, comma separated: `name` or `name:protocol` |
| `needs` | no | ports required *before* it starts — this is what fixes creation order |
| `grants` | no | non-port custody: `irq-control`, `dtb`, later device frames |
| `spawns` | no | the entries (or `prefix*` classes) this service may create, defaulting to none — a spawn right, not a wish |
| `restart` | no | `always`, `on-fault` (default), `never`, with optional backoff |
| `priority` | no | scheduling priority |
| `args` | no | the `argv` it is started with; a list, never a shell |

### Validation, and failing loudly

Director validates before it creates anything, and an invalid manifest stops the
boot with a message rather than starting a partial system:

- **unknown keys and unknown sections are errors.** A silently-ignored line is
  how a typo turns an `authority` declaration into a service running with the
  wrong authority — the one class of mistake this file must never swallow.
- every `binary` names an existing initrd entry, exactly;
- every `needs` names a port that some entry `owns` (ownership is declared, so
  this is answerable statically), and the resulting graph is acyclic — the graph
  *is* the creation order;
- port names are unique system-wide;
- every `spawns` names entries that exist, and no entry may spawn something with
  more authority than it holds itself;
- a `user` entry may not declare `grants`, `irq-control` or device custody;
- `format` must be a version director understands.

Where the manifest lives in the repo, and whether it stays one file or becomes
one per service, is an open item below.

## Ports

A port is what a service offers to the rest of the system: an seL4 endpoint
capability with an owner, a name and a protocol.

- **Mechanism.** Request/reply over an endpoint — `seL4_Call`, `seL4_ReplyRecv`,
  `seL4_Recv`, `seL4_Send`, `seL4_NBSend`
  (`kernel/libsel4/include/sel4/syscalls_master.h:70`, `:125`, `:51`, `:27`,
  `:100`); one-way signalling through notification objects — `seL4_Signal`,
  `seL4_Wait` (`:178`, `:200`). A server's loop is `seL4_ReplyRecv`; a message is
  a method identifier plus inline words plus an optional out-of-line buffer.
- **Identity.** The badge the kernel reports on receipt — `seL4_Recv`'s `sender`
  parameter is documented as "the badge of the endpoint capability that was
  invoked by the sender"
  (`kernel/libsel4/include/sel4/syscalls_master.h:37-43`) — minted when the
  capability is created (`seL4_CNode_Mint(..., badge)`,
  `out/aegir/libsel4/include/interfaces/sel4_client.h:2415`). "Who called me" is
  therefore answered by the kernel, not by a field a sender could lie about.
- **Names.** `class.name` — `log.main`, `vfs.namespace`, `devmgr.registry`,
  `blk.virtio0`. The manifest binds names to owners, and at boot **that binding is
  the capability distribution**: no lookup service exists, because every
  dependency is declared before anything starts. When the config service lands,
  the same names become registrable at runtime; until then the manifest is the
  answer to "who owns this port".
- **Protocols** are named per port (`name:protocol`) so a client can state what it
  expects to be talking to; the wire format belongs to the service that owns the
  port, and is specified there rather than here.
- **The C++ API is deliberately not specified in this file.** Headers, marshalling
  helpers and the call/reply ergonomics are `libs/aegir-ipc`'s job in the code
  milestone. What is fixed here is the model: ports are owned, named, badged and
  versioned, and a capability is the only way to reach one.

### Making a port exist

The model above says what a port *is*; the spawn path has to make one, and these
four choices are what that means. They are decisions, not implementation details,
because both sides of every port depend on them:

- **Director creates every port the manifest declares**, holding the endpoint so
  that both sides get a capability to the same object without either having to
  hand the other anything. A service never grants access to itself.
- **Rights are per side, and they are what make a port's direction real.** The
  owner gets Read: it receives, and a *reply* needs nothing from the endpoint,
  because the kernel hands the callee a reply capability instead. A consumer gets
  Write -- and, if it *calls*, whatever else the kernel requires of a callable
  capability. That last part is not a free choice: the kernel states the rule for
  fault endpoints as "both Write rights and either Grant or GrantReply"
  (`out/aegir/libsel4/include/interfaces/sel4_client.h:1202`), and the exact
  minimal set for a caller is confirmed when `libs/aegir-ipc` is written and
  checked at boot, so that it cannot quietly rot.
  The narrowness is the point: **any Read holder can receive**. A consumer with
  Read could take a call meant for the owner, because the kernel hands a message
  to whichever receiver arrives first. Direction is therefore a capability fact
  rather than a convention -- and it is why the owner does not get Write either: a
  port that only its consumers may send to is a simpler thing to reason about
  than one that anyone holding a capability may.
- **Slots are declared, not discovered.** A service's ports -- the ones it owns and
  the ones it needs -- start at `kSlotFirstDeclared` and go upward in manifest
  order, so the layout is a reading of the manifest (`specs/director.md`).
- **A child finds a port by name, through the bootstrap block.** The block carries
  one `Capability` entry per port: the name, and the slot it was installed in.
  That is what the entry kind was for, and it keeps the *name* the identity while
  the slot stays an artifact of a layout the child did not choose.
- **Creation order is the graph.** Services are created in `needs` order, so a
  port's owner is always running before its first consumer. A consumer is never
  handed a capability to something that does not exist yet -- and if the graph has
  a cycle, nothing is created at all and the boot says so.
- **A port has exactly one reader.** Ownership *is* the read side: the service
  that `owns` a port is the only one that may receive on it, and everyone else
  gets Write, for the set of ports the manifest declares with `needs`. This is the
  rule that closes the hole a shared read side would open. seL4 delivers a message
  to whichever receiver arrives, so **any** Read holder can take a call meant for
  someone else -- and the sender cannot tell, because a thief that replies is
  indistinguishable from the owner it impersonated. Not sharing Read makes that
  impossible rather than merely impolite. The manifest already guarantees it: port
  names are unique system-wide, so exactly one entry owns each port and nobody
  else can be given Read to it.

### Callbacks are the other direction, not a wider grant

A callback-style protocol -- the server answering later, or asking its client
something -- needs no shared read side. It needs a *second port*, pointed the
other way: the client owns `answer`, and the server only writes to it. The
manifest already says which is which, because ownership and `needs` name the
direction of every port. With that, the symmetric case is two ports and the
synchronous case is one, and `Call`'s reply needs neither: the reply capability is
created by the kernel in the callee's CSpace, so it can be neither forged nor
stolen.

What makes this cheap is that nothing has to be handed over at run time. Whoever
spawns a process installs its ports into its CSpace, so the device manager gives
each driver it starts an event port of its own -- exactly as director does for its
children -- and the driver's answers come back on a port the driver owns. Capability
transfer (`Grant`, `GrantReply`) is therefore not needed for the boot set at all,
which is a far narrower question than it first looked.

### The boot set's protocols

A port's wire format belongs to the service that owns it, so a service with a
protocol worth specifying gets its own spec. The first one is small enough to fit
here, and writing it down is the point: an ABI that lives only in the head of
whoever wrote it is a bug waiting for a second caller.

**`log.main`, version 1** -- owner `logger`.

| | |
| --- | --- |
| Call | method `1` *event*: one argument, an event code |
| Reply | one word: `0` recorded, non-zero otherwise |
| Identity | the caller's badge, which the logger reports as the source of the line |

Deliberately **words, not strings**. A line of text is an out-of-line buffer, and
the buffer convention is worth designing once something needs to send one rather
than invented by the first caller who wants to print. An event code plus the
caller's badge is enough to say "this service reached this step", and it keeps the
first protocol small enough to be right.

Version 2 is where strings land. The method number is what makes that possible
without a flag day: an owner that does not know a method replies with an error
that the caller can read.

## The boot set

| # | Service | Authority | Owns | Needs | Why it can start then |
| --- | --- | --- | --- | --- | --- |
| 1 | `logger` | system | `log.main` | — | everything else logs; the only thing that keeps using the raw debug console after boot |
| 2 | `device-manager` | system | `devmgr.registry` | `log.main` | holds `IRQControl` custody and the DTB copy; needs no filesystem; launches block drivers |
| 3 | `vfs` | system | `vfs.namespace` | `log.main` | must exist before any filesystem, so filesystems have somewhere to register |
| 4 | `partition-manager` | system | `partman.partitions` | `log.main`, `devmgr.registry` | needs block devices to exist; launches one filesystem per partition |
| 5 | `auth` | system | `auth.login` | `log.main`, `vfs.namespace` | needs the user database, which lives on a volume that only exists after 2-4 |
| 6 | *sessions* | user | — | `log.main`, `auth.login` | not part of boot proper: `auth` asks director for one per authenticated user, and it starts with user authority |

Every `needs` above names a port that some row `owns` — which is what makes the
table a valid manifest sketch rather than prose: a row consuming a port nobody
declares fails validation before anything is created.

Ports a service creates *after* it starts are not manifest entries. A filesystem's
per-volume port, a session's ports: those are registered at runtime — with the VFS
today, through the config service later — and reachable by whoever is handed the
capability. The manifest declares the ports that must exist before a service
starts, which is exactly the set that has to be resolvable statically.

### Creation order is static; availability is not

The ordering above is a *port* graph, and the manifest can only express that much.
`auth` needs a user database, which lives on a volume, which comes from a
filesystem, which comes from the partition manager, which comes from block
drivers. Ordering cannot fix that, because a disk can take a second to answer.

So: **the manifest fixes creation order through port dependencies and nothing
else. Anything a service needs that is not a port, it waits for.** A service that
has found what it needs reports `ready`; director's boot report waits for every
entry to be ready, and reports the stragglers instead of hanging. "Ready" is the
word that matters here — a service that is merely running has not finished
booting.

## The device manager

Its job is one mapping: **bus → device id → the service process that handles it.**
That map is the reason it exists, and everything else it does is in service of it.

- **Source of truth.** The device tree, granted by director (the DTB copy from the
  extended bootinfo, `specs/director.md`). Platform devices are described there.
  PCI, when it comes, is a bus we must *ask* rather than read, and it feeds the
  same registry with the same shape — that is the test of whether this design is
  right.
- **What it decides.** For each device it recognises — by compatibility string,
  then by instance — which driver service handles it and under which instance
  name (`blk.virtio0`).
- **Least authority.** A driver gets exactly its device's MMIO frames and an IRQ
  cap minted from `IRQControl`, whose custody is delegated to this service at
  boot. Upstream
  shows the shape of that grant in `sel4platsupport_copy_irq_cap`
  (`projects/seL4_libs/libsel4platsupport/include/sel4platsupport/device.h:50`),
  which installs an IRQ handler capability into a *target* CSpace — a driver's,
  not the grantor's.
- **Launching: the device manager launches its drivers.** Its manifest entry is
  the authority for this — it declares a spawn right over the driver entries
  (`spawns = blk.*`, say) and holds a pool to build address spaces from — so when
  it recognises a device it starts the driver for it directly, passing the
  instance name as an argument and charging the driver to an account. Decision and
  act in one process, on purpose: the service that knows what should exist is the
  service that creates it, and a device appearing later does not need a privileged
  round trip to be handled.
  The alternative — routing every spawn through director — is rejected for that
  reason: it would make the device manager a requester rather than the owner of
  its drivers, and put the reason a driver exists in a different process from the
  device that needs it. What bounds the device manager is what bounds every
  spawner: the authority it was granted, and no more.

## The partition manager and the VFS

- The partition manager asks the device manager's registry for block devices,
  reads the partition table (MBR/GPT — a filesystem-agnostic job), and for each
  partition starts the filesystem service matching that partition's type — a
  spawn of its own, under the spawn right its manifest entry declares.
- A filesystem service receives the block device's port and a *range* grant
  (offset and length), not the whole device. Least authority again.
- **Filesystems register with the VFS.** `vfs.namespace` accepts a registration
  carrying a volume name, the filesystem's port and what it can do. This is the
  Amiga `FileSystem.resource` pattern: filesystems announce themselves to the OS
  instead of being compiled into it, which is what makes "add a filesystem" a new
  service rather than a new OS.
- The VFS owns the **namespace** — volume names, path resolution, who may look up
  what — and not the data. Files live in the filesystems; the VFS is the map.
- **`Initrd:` is the first volume.** Director holds the flat archive already, so
  it can register it as soon as `vfs.namespace` exists (order 3, before
  partition-manager at 4) and every later service can read the boot image through
  the namespace. That is what lets `auth` start with an initial user database from
  the initrd and switch to the authoritative one on the root volume once it
  appears.

Amiga ancestry, recorded as inspiration rather than mechanism: `expansion.library`
and `BindDrivers` for a device manager that decides which driver binds to what,
`mountlist` for partition → filesystem, `FileSystem.resource` for filesystem → VFS
registration. The names are different because the mechanisms are.

## Lifecycle

- **Start** is the spawner's: director for the boot set, the device manager for
  drivers, the partition manager for filesystems, a session for user processes —
  in the order the manifest's `needs` graph gives.
- **Ready** is the service's own report, sent when it has what it needs.
- **Fault** goes to the fault endpoint its spawner installed, badged with the
  offender.
- **Restart** is the manifest's policy, applied by that spawner: `always`,
  `on-fault` or `never`, with backoff.
- **Failure propagates along `needs`.** The graph that fixes creation order is
  also the failure graph: a service whose dependency is gone does not limp along,
  it is stopped, and restarted with it if the policy says so.
- **Shutdown** does not exist yet: no orderly stop, no power management, no
  supervision of director itself (`specs/director.md`).

## Open, for review

- **Where the manifest lives in the repo.** Settled by the first implementation:
  `manifests/services.manifest`, one file, packed into the initrd under its
  basename (`services.manifest`), which is how director finds it. One file rather
  than one per service for now, because director reads one entry and the set is
  small; the cost of the alternative is reading several flat entries, and the
  benefit is a per-service diff. Revisit when the file stops being readable in
  one sitting — the fields do not change either way.
- **The grammar's details** (list syntax, quoting, whether a protocol is part of
  the port name or a separate key) are fixed with the parser; the fields above are
  fixed here.
- **The granularity of a spawn right** — naming entries exactly (`blk.virtio0`) or
  by class (`blk.*`). Classes save a manifest edit per device and need a matching
  rule; exact names are unambiguous and make a new device type a manifest change.
  Proposal: class patterns, matched literally up to the `*`.
- **May ports carry capabilities?** `Grant` and `GrantReply` are what let a
  capability travel inside a message or a reply. With callbacks as reverse-direction
  ports (above), nothing in the boot set needs it: every port a process holds was
  installed by whoever spawned it, and a driver's devices are installed the same
  way. What would need it is two processes that did not spawn each other handing
  each other something at run time -- hot-plug, or a shell passing a port to a
  service it did not start. When that arrives, it belongs to *individual* ports (a
  protocol that says it carries capabilities) rather than to every port by
  default.
- **Who registers `Initrd:`** — proposal: director, the moment the VFS is up.
- **Hot-plug** (a device appearing later) and **device removal** are unmodelled;
  both end up as registry updates plus spawn/stop requests.
- **What a session is made of** — the services an interactive user actually gets
  — is a later spec.


## The device manager

The boot set has a logger because something has to be first. It has a device
manager because **something has to know what the machine is**, and because that
knowledge is what lets Aegir start a driver without a human telling it which
irq belongs to which disk.

The shape is the one the requirements ask for: the device manager holds a
**bus -> device id -> service** map. A bus is a way of naming devices (memory-
mapped virtio transports, later PCI), a device id is how that bus names one of
them, and the map says which service is responsible for it. Devices arrive from
somewhere and services are launched for them, so the map is what turns "there is
a virtio block device at `0x10003000`, interrupt 4" into "the virtio-blk driver
now owns that".

### The device tree is the bus report

Nothing has to be discovered by poking at hardware to begin with: the firmware
told us. The device tree blob is where the machine's devices, their register
windows and their interrupts are written down, and the kernel hands it to us --
the elfloader places it in the extra bootinfo pages and the kernel retypes those
pages into frame capabilities in the root task's CSpace
(`seL4_BootInfo::extraBIPages`, kernel/libsel4/include/sel4/bootinfo_types.h:68).

So the first thing Aegir does with a device is *read* it: director maps those
pages with the scratch window (libs/aegir-mem, `Scratch::map`, which exists for
exactly this -- mapping a frame we hold a capability for), finds the blob's
magic, and reads it with our own device tree reader (libs/aegir-devtree). No
hardware is touched, and a device the tree does not mention is a device we do not
have.

### What director does, and what the device manager will do

Director reads the tree because it is the only task that can: it holds the
capabilities for those pages, and it is the one that creates services. What it
finds it reports (`device tree: ...`, one line per device the tree describes).
The **device manager** is a service that owns that report and acts on it: it is
the process that holds the map, launches the drivers for the devices it finds,
and answers "who owns this device?" for everyone else.

The device manager is not spawned yet; this milestone is the foundation it needs,
and saying which half exists is the point of writing it down:

- **exists**: director maps the device tree and hands it to the device manager
  through the bootstrap block (a `Devices` entry: the blob's address in the child's
  own address space, and its size). The device manager reads it with our own reader
  (libs/aegir-devtree) and reports the buses it describes;
  the tree must be *moved* into the scratch window rather than mapped there, because
  the kernel has already mapped the extra bootinfo pages into the root task and a
  frame cannot be mapped at two addresses -- the kernel says so out loud
  (`RISCVPageMap: attempting to map frame into multiple addresses`);
- **device memory, measured**: a device's registers are untyped like any other
  memory, marked as device, and a device frame can only be retyped from one.
  `Untyped_Retype` takes no interior offset, so it carves from the untyped's own free
  position and a page is reached by retyping every page before it. On this machine the
  device space begins where the devices do: one device untyped at `0x10000000`, so the
  transports the tree names (`0x10001000` to `0x10008000`) are pages 1 to 8 of it, and
  page 0 is not a transport at all. The retype's depth must be `seL4_WordBits` -- the
  root task's CNode has a guard, so anything less comes back as `seL4_FailedLookup`
  (the allocator's `kRootCNodeDepth` is `seL4_WordBits` for the same reason).
- **a dropped frame goes back to its untyped, and the next retype carves it again**:
  retyping in a loop into *one slot* and deleting between rounds yields the untyped's
  first page every time. That mistake read as "the device answers zeros" for three
  attempts. The pages along the way are kept, each in its own slot, and the boot says
  how many retypes it took to reach the device.
- **a device's registers read, and the boot says which transport has one**:

      device memory: transports from 0x10001000 to 0x10008000, ...
      device at 0x10008000: magic 0x74726976, device id 4
      transports: 1 of 9 device pages have a device behind them

  `0x74726976` is "virt", and the device id is what says whether anything is behind a
  transport: an empty one answers the magic, version 1, vendor `0x554d4551` and
  **device id 0**. Device id 4 is an entropy source, which is what the run gives the
  machine with `-device virtio-rng-device` (scripts/targets.py; no backing file
  needed). Only the pages between the two ends the tree names are read, and every page
  in that span is a transport, which is what makes reading them safe.
- **a device lands on the transport the tree names *first***, measured: the tree lists
  the transports in descending order and QEMU creates them in that order, so the
  device answers at `0x10008000` -- the *highest* address. The guess that QEMU fills
  them from the bottom up was wrong, and acting on it made director prefer the one
  transport with *no* device behind it. The boot no longer claims to know which
  transport is busy; the survey reads, and says.
- **what seL4 requires to map a device frame, from the manual and the kernel**: nothing
  device-specific. On RISC-V the only attribute bit is `seL4_RISCV_ExecuteNever` and
  `seL4_ARCH_Uncached_VMAttributes` is *aliased to zero*
  (projects/seL4_libs/libsel4vspace/arch_include/riscv/vspace/arch/page.h:38), the map
  decoder has no device test at all
  (kernel/src/arch/riscv/kernel/vspace.c:801-931, `decodeRISCVFrameInvocation`), and
  rights are only ever masked against the capability's own
  (`vspace.c:899`, `maskVMRights` at `:634-652`) -- so a device frame maps like any
  other, writable, with `seL4_Default_VMAttributes`. What *does* constrain it is the
  rule in the manual: **a frame capability can be mapped into one VSpace only**, and
  sharing a page means duplicating the capability with `seL4_CNode_Copy` and mapping
  the copy (kernel/manual/parts/vspace.tex:367-373; the checks are `vspace.c:867-881`,
  returning `seL4_InvalidCapability` for another address space and
  `seL4_InvalidArgument` for a second address in the same one). Upstream maps its
  serial device exactly this way -- a device frame, `seL4_AllRights`, attribute zero,
  into the root task's *own* VSpace
  (projects/seL4_libs/libsel4platsupport/src/common.c:86-113) -- and sel4test gives a
  device frame to a *child* process by copying the capability
  (projects/sel4test/apps/sel4test-driver/src/testtypes.c:241-243).
- **a device is not yet given to a service**: the pieces are in place and compiled --
  a `Device` block entry (address and size, separate from the `Devices` blob), a
  `Request` field the spawner maps above the blob, and the device manager reading its
  own device's magic and device id -- but the mapping into the child comes back
  `seL4_InvalidCapability`, and the kernel says which branch: its console line is
  `decodeRISCVFrameInvocation/871` -- `RISCVPageMap: Attempting to remap a frame that
  does not belong to the passed address space` -- which is
  kernel/src/arch/riscv/kernel/vspace.c:871, the check that the frame's
  `capFMappedASID` equals the VSpace's ASID. So the frame still carries a valid ASID
  at the moment the child's VSpace is given it.

  Measured, and this is the part that narrows it: **the copy of the device frame maps
  into our own VSpace** (`device probe: the copy maps into our own space, so the
  child's is what refuses`), and the probe's mapping is removed immediately afterwards
  so it cannot cause the failure it tests for. The copy is therefore fine and the
  child's map is what refuses -- which does *not* mean the child's VSpace is broken,
  because every RAM frame maps into it.

  What `performPageInvocationUnmap` does is now known
  (kernel/src/arch/riscv/kernel/vspace.c): it clears `capFMappedASID` **on the
  capability the unmap is invoked on**, and unmaps the page-table entry only when that
  capability says it is mapped. It does *not* clear other capabilities to the same
  object, and it does not refuse when there is more than one.

  With that known, the survey now unmaps each page as soon as it has read it, so
  nothing in this path leaves a mapping behind -- and the child's map is *still*
  refused at vspace.c:871. A temporary print in that branch (reverted) said exactly
  which side is which:

      [aegir] remap: frame_asid=2 asid=3 asidInvalid=0

  Director is ASID 2 and the child is ASID 3, so the frame belongs to *our* address
  space at handover, with every unmap in this path having been called. The next
  instrument is therefore on the other side of the unmap: a print inside
  `performPageInvocationUnmap`, to see whether it is reached for each of those unmaps
  and which capability it clears. That is the last thing between here and a service
  reading its own device.

- **next**: the bus -> device -> service map inside the device manager, and
  spawning drivers (virtio-blk first) for the devices it finds, giving each the
  device's register window and interrupt. The service exists and reports the
  machine; what it does not have yet is anything to *serve*, which is why it owns
  no port and why its report goes to the console.

Devices are given to a driver the way everything else here is given: capabilities
for the device's register frames (retyped from the device's own untyped memory --
device frames cannot be retyped into anything else,
`kernel/src/object/untyped.c` refuses a non-page type on a device untyped) and an
interrupt handler (`seL4_IRQControl_Get`, `seL4_IRQHandler_SetNotification`,
`seL4_IRQHandler_Ack`) whose notification the driver waits on. A driver therefore
does not need to be told about the machine -- it needs to be told about *its*
device, and the map is what works out which is which.
