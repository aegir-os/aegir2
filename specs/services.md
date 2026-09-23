# Services, ports and the boot manifest

Status: implemented except where an open section says otherwise (2026-09)
— the manifest, the boot set, the storage stack and the sessions row stand;
what remains is the open list at the end.

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
| `delegate_mib` | no | the MiB of untyped memory a spawning service is delegated, rounded up to a power of two — absent means the default (4) |
| `restart` | no | `always`, `on-fault` (default), `never`, with optional backoff |
| `priority` | no | scheduling priority |
| `args` | no | the arguments after `argv[0]` (which is the service's name); a list, never a shell |
| `environment` | no | the `NAME=VALUE` items it starts with, comma separated (specs/environment.md) |
| `cwd` | no | its current directory, a VFS path; absent means none (specs/environment.md) |

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
  One convention the badge space has now grown: a server that also waits on a
  bound notification (a supervisor that serves) cannot tell a call from a
  signal by the message length — a bound notification's delivery sets the
  badge register and nothing else
  (`kernel/src/object/notification.c:62-76`), so the length is stale from the
  last reply — and a caller's badge and its signal's badge are the same
  number, the service's own. So a call to a port the supervising server owns
  carries the caller's badge with the top bit set, and a signal arrives bare;
  both sides of the convention are the owner's to keep, since it badges the
  caller caps it hands out and it checks.
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
| 4 | `initrd` | system | `vol.initrd` | `log.main`, `vfs.namespace` | serves the boot image as `Initrd:`; needs somewhere to register, and nothing else |
| 5 | `partition-manager` | system | `partman.partitions` | `log.main`, `vfs.namespace` | needs block devices to exist (the registry and their ports arrive as grants); launches one filesystem per partition |
| 6 | `console` | system | `console.gui` | `log.main`, `devmgr.registry` | needs the display and the HID devices bound; needs no filesystem (`specs/console.md`) |
| 7 | `test` | system | — | `log.main`, `vfs.namespace` | the accumulating test bed; asks again until the volumes it checks exist |
| 8 | `auth` | system | `auth.login` | `log.main`, `vfs.namespace`, `console.gui` | needs the user database, which lives on a volume that only exists once the filesystems serve; spawns the greeter on the console (`specs/auth.md`) |
| 9 | *sessions* | user | — | `log.main`, `vfs.namespace`, `console.gui` | not part of boot proper: `auth` starts one on each successful login, with the user's badge and account (`specs/auth.md`) |

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

- The device manager starts the partition manager once block drivers answer, and
  hands it their ports -- the registry is the map it just built, not a service to
  ask. The partition manager reads the partition table (GPT first — a
  filesystem-agnostic job), and for each partition starts the filesystem service
  matching that partition's type — a spawn of its own, under the spawn right its
  manifest entry declares.
- **One partition type GUID is ours**: `5cd58811-9bf5-4af3-8682-9b76edce3535`
  names the Aegir system volume — the discovery shape of systemd's
  Discoverable Partitions Specification, one GUID per role, the disk
  describing itself rather than a name string or an attribute bit saying
  it. The partition manager reads it from the entries it already walks, and
  the volume it yields registers with the boot flag, which the VFS aliases
  as `Sys:` (`specs/vfs.md`). `make_disk.py` types the first partition with
  it (sgdisk `--typecode`); a disk with none has no `Sys:`. The GUID is a
  statement about the *partition*, not the filesystem in it — FAT serves it
  today, and BeFS inherits the alias the day it lands.
- A filesystem service receives the block device's port and a *range* grant
  (offset and length), not the whole device. Least authority again.
- **Filesystems register with the VFS.** `vfs.namespace` accepts a registration
  carrying a volume name, the filesystem's port and what it can do. This is the
  Amiga `FileSystem.resource` pattern: filesystems announce themselves to the OS
  instead of being compiled into it, which is what makes "add a filesystem" a new
  service rather than a new OS.
- The VFS owns the **namespace** — volume names, path resolution, who may look up
  what — and not the data. Files live in the filesystems; the VFS is the map.
- **`Initrd:` is the first volume.** A tiny `initrd` service (binary
  `aegir-fs-initrd`) holds the flat archive and registers it as soon as
  `vfs.namespace` exists (order 4, right after the VFS at 3), and every later
  service can read the boot image through the namespace. That is what lets
  `auth` start with an initial user database from the initrd and switch to
  the authoritative one on the root volume once it appears.

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
- **May ports carry capabilities?** Decided with the console arc (2026-09):
  **yes, for individual ports whose protocol says so**, never by default.
  `Grant` and `GrantReply` are what let a capability travel inside a message
  or a reply; the first protocol to say so is `console.gui` -- attaching
  hands the client its pixel slice's frame caps one per reply, and
  `create_window` answers with the window's minted event endpoint
  (`specs/console.md`). The registry's `open` is the precedent: a minted cap
  in a reply, and the gpu window's frames ride the replies the same way, one
  cap at a time. What remains true: nothing carries capabilities without
  declaring it, and two processes that did not spawn each other handing each
  other something at run time -- hot-plug, a shell passing a port -- is a
  protocol's stated property, not a facility every port grows.
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
- **a service reads its own device**: the device manager is given the device's
  registers and reads them itself, which is the first time anything but the root task
  touches hardware:

      device at 0x10008000: magic 0x74726976, device id 4          (director's survey)
      devicemgr running at 0x1016a, badge 2
          my device at 0x25000: magic 0x74726976, device id 4  (virtio: the magic reads)
      devicemgr ready
          my device: none was given                                 (hello, which is not)

  Three things make it work, and each was wrong first: the frame is reached by keeping
  the pages before it (a dropped frame goes back to its untyped); the retype's depth is
  `seL4_WordBits` (the root CNode has a guard); and the device goes to **exactly one
  service**, because a device is a capability and mailing it to every spawn lets the
  first service take it and refuses the rest -- which is what `seL4_InvalidCapability`
  meant all along, "a frame that does not belong to the passed address space". Which
  service that is comes from the manifest's `device_manager` field, because that is
  where composition is declared; the device *tree* is given to the same service for
  the same reason, and it was being over-shared in exactly the same way.
- **the map's first content, said by the service that owns it**: the device manager
  names every transport the tree describes and marks the one it was given:

      devicemgr running at 0x1016a, badge 2
          my device at 0x25000: magic 0x74726976, device id 4  (virtio: the magic reads)
            virtio,mmio 0x10008000  <- mine irq 8
            virtio,mmio 0x10007000 irq 7
            ... all eight the tree names ...
      devicemgr ready

  Which is why the `Device` block entry carries *two* addresses: `number` is the
  device's physical address -- identical transports are told apart only by where they
  are, so a service that drives one has to be told *which* -- and `data_offset` is
  where in its own address space it can read them.
- **a transport's version register does not settle which register layout it answers on**: the
  block driver's first queue probe read `QueueNumMax` at the *legacy* offset (0x030) after
  selecting with the legacy `QueueSel` (0x02c) and got 0 and 0, which is a device that has not
  been asked anything. Writing the *modern* `QueueSel` (0x030) and reading the *modern*
  `QueueNumMax` (0x034) answered 1024 -- on a transport whose `Version` register reads **1**.
  So the version is a claim and the registers are the evidence, and the driver now asks both
  layouts and reports which one replied:

      queue: size 1024, on the modern layout

  This is the same lesson as "a device lands on the transport the tree names first": the
  question is not what the documentation says the machine is, it is what the machine answers.
  The first probe's 0 and 0 was the useful outcome -- an offset that is wrong says so, where a
  queue built in the wrong place would have failed later as a request that never came back.
- **a driver needs memory whose physical addresses it knows, and does not have any**: a
  virtqueue's descriptor entries carry *guest-physical* addresses (`struct vring_desc.addr`,
  projects/util_libs/libvirtio/include/virtio/virtio_ring.h:63-72), and the modern layout takes
  the queue's addresses in registers (QueueDescLow/High at 0x080/0x084, QueueDriver at
  0x090/0x094, QueueDevice at 0x0a0/0x0a4). A service can be given device frames and can map
  them, but nothing in that tells it a *physical* address. So a driver needs what the device
  manager already has -- an untyped to retype frames from, whose physical base is known
  -- plus a way to be told that base. Done: the manifest asks for memory the way it asks
  for a device, the spawn path maps the frames and records the region's physical base and
  size in the block's `untyped` entry, and a spawner that carves the memory itself (the
  device manager, for its driver) records the base of its own carve the same way.
- **a virtqueue is written, and the device answers**: the driver lays out a queue -
  descriptor table, available ring, used ring, request header, one sector of data, a status
  byte, in the two pages the spawner mapped - publishes a read of sector 0, notifies, and
  polls the used ring. The device publishes a used entry. Getting there took a chain of
  measurements, each one replacing an assumption:

  - `VIRTIO_F_VERSION_1` (feature bit 32) has to be negotiated once the device offers the
    modern register layout; offering no features at all leaves a queue set up and ignored.
  - the device reports version 1 and yet answers the *modern* register layout at 0x030/0x034,
    while keeping *none* of the modern queue registers: `QueueNum` reads back 0, `QueueReady`
    0, `QueueDescLow` 0 after being written.
  - **the used ring is a page after the rest of the queue.** `vring_init` puts it at
    `align(&avail->ring[num] + sizeof(uint16_t), align)` and the alignment is 4096 --
    `VIRTIO_PCI_VRING_ALIGN`, "the alignment to use between consumer and producer parts of
    vring" (projects/util_libs/libvirtio/include/virtio/virtio_pci.h:92). 150 rounded to 4096
    is a second page, so the queue needs two, and a device writing its used entry at base+4096
    had been writing outside the single page the driver polled.
  - **DRIVER_OK comes after the queues are set up.** It is the driver saying everything is
    ready (virtio 1.x, 2.1.1 step 8); the driver had been writing it *before* configuring the
    queue.
  - **RISC-V orders stores weakly**, so the available index needs a release fence on either
    side of it, which is what the in-tree legacy driver does around its own `avail->idx++`
    (projects/util_libs/libethdrivers/src/virtio_pci.c:286-289).

  - **the device offers no `VIRTIO_F_VERSION_1`, so it is the legacy interface.** Read back from
    the device's own feature word: bit 32 is not offered, and writing it to the driver's word
    does not make the device keep it. The earlier "1024" from 0x034 was the legacy `QueueNum`
    reading its default, not a modern `QueueNumMax` answering. The handshake itself is right --
    the status after it reads 11, ACKNOWLEDGE|DRIVER|FEATURES_OK.
  - **the legacy shape does take, and the device still does nothing.** `QueuePFN` reads back as
    the page frame of the physical address given, and the request is ignored completely: the
    status byte is seeded with 0xff and comes back 0xff, the data buffer is untouched and no
    used entry appears. So the queue's location is *accepted* without the device ever *reading*
    the rings, and what is left is the legacy interface's own rules for when it will.
  - **a request that is ignored leaves no trace at all**, which is worth more than a wrong
    answer would be: seeding a byte with a value the device must overwrite turns "the read did
    not complete" into "the device did not look", and those are different searches.
  - **the device was never looking at the driver's memory.** Every hypothesis above was about
    what the driver told the device; the answer was in what the driver was *told*. The kernel
    carves an untyped's children from the low end of what is left and advances its free index
    (`kernel/src/object/untyped.c:225-232`, `:294-302`), so after a region is split down, the
    piece that remains -- the one `carve_untyped` hands out -- sits at the region's *far* end,
    not at its base. The allocator's bookkeeping said the opposite, so the driver wrote its
    rings into one page and gave the device the physical address of another: an all-zero avail
    ring is a ring with nothing to do, which is a device that never answers, with no error and
    no interrupt. Nothing else could see it because the recorded physical address has exactly
    one consumer -- a device doing DMA; everything else only retypes objects, which is correct
    regardless. The fix is in `split_to` (libs/aegir-mem/src/allocator.cc), and the boot after
    it reads:

        my memory: 8192 bytes at 0x24000 (physical 0xffff0000)
        queue: num 0 of 1024, ready 0, desc 0x0, legacy, pfn 0xffff0 (was 0x0)
        read sector 0: status 0 (0 is ok), 513 bytes used, first 16: 0x0 ...

    513 is the whole device-writable length of the chain -- 512 bytes of sector plus the status
    byte -- and the zeros are the disk image's real content (a sparse megabyte,
    scripts/run_target.py). The read of sector 0 completes: the device was talked to correctly
    end to end, and the thing it could not do before was find the queue.
- **a service cannot map into its own address space, so its spawner maps for it**: the spawner
  retypes the child's root page table, assigns it to an ASID pool, and keeps the capability.
  What a child is *given* is its TCB, its CNode, the fault endpoint, the supervision
  notification and its ports -- and not its VSpace root
  (libs/aegir-spawn/src/process.cc:351-380; `ChildVSpace` maps through the `root_` the spawner
  holds). Two consequences: `Scratch` cannot help a service, because it derives its window from
  a bootinfo a child does not have; and a service that retypes a frame from its own untyped
  cannot then map it. **The spawner maps for the child**, which is what `Request.device_frame`
  already does for a device window -- so a driver's virtqueue page is the same shape of
  request: director retypes the page from the system account, maps it into the child, and says
  its physical address. The untyped a service is given stays useful for authority -- what it
  may make -- but the mapping of what it makes has to come from the spawner.
- **a service can be given memory and told where it is**: `memory_kib` in a manifest section
  makes director carve an untyped from the system account, hand over the capability, and say
  the region's *physical* base, because a virtqueue's descriptor entries are guest-physical
  addresses the *device* reads and no invocation tells a service where its own memory is. The
  block carries it as its own entry kind (`number` = physical base, `reserved` = size in bits),
  beside the `Capability` entry that already carried the size:

      blkdriver: binary aegir-virtio-blk, authority system, account system, memory 4 KiB
          my memory: 4096 bytes at physical 0xfff1d000, capability 9

  **The bug in that was an index, and the count was right anyway**, which is why it took a
  probe rather than arithmetic: adding the entry made the block's fixed count 7, but the loop
  that writes the ports still started at 6, so the *first port overwrote the new entry*. The
  entry count was correct, the capability search was correct (it matches on kind, not on
  index), and everything worked except the one entry that had been overwritten. Reading
  `entries[6]` from the child said `kind 4` -- a `Capability` -- where 8 was expected. Any
  fixed entry added before the ports has to move the port base with it.
- **done**: where a driver's shared parts go. The first driver, virtio-blk, has a
  register window and a status handshake that every virtio device on the
  bus shares -- the layout is the same ABI for all of them (virtio 1.x, 4.2.2), and the
  handshake is: reset, ACKNOWLEDGE, DRIVER, features, FEATURES_OK, verify, DRIVER_OK --
  while only the config space and the request queue are the device's own. The second
  driver was virtio-rng, the smallest device on the bus (one queue, and the device only
  ever writes into buffers the driver posts), and its arrival split the shared
  parts into **`libs/aegir-virtio`**: the register window, the handshake, and the
  virtqueue core (descriptor table, the two rings, publish and wait -- a `Queue`
  object with its own cursors and interrupt pairing, because the third driver,
  virtio-input, has two queues), with each driver keeping only its device's own
  shapes. The seam is the legacy-MMIO transport made concrete; a second
  transport (virtio-pci's modern layout, if PCI is ever taken on) arrives
  *beside* it, not through it -- the ring and the device logic are the shared
  parts, the register map and queue activation are the transport's.
- **next, and designed**: give the device manager the *series* of transport frames, so
  it can inspect every transport rather than only its own. The shape:
  - the survey already retypes and keeps every page of the series, one slot per page;
    what it must also report is the *first* slot, the page count, and the base
    physical address (`pages` and `base` are both in scope where the loop starts).
  - `Request.device_frame` becomes the first of `device_frame_count` capabilities in
    **consecutive slots**, which is how the allocator hands slots out
    (libs/aegir-mem, `alloc_slot`) and is what lets the spawner map them without a
    list: `device_frame + i` is page `i`'s capability.
  - `device_bytes = count * 4096`, and the child's window offsets equal the machine's,
    so a service can work out which device is at which offset.
  - the `Device` block entry already has what is needed (address in `data_offset`,
    size in `length`) and the *physical base* of the window in `number`, so a service
    knows where its window is in the machine.

  **The trap, paid for once**: `bootstrap::write` now puts a device's *physical*
  address in `number` and its *child* address in `data_offset`, and the readers
  (`bootstrap::devices` and `bootstrap::device`) read different fields of the same
  entry kinds. Editing one without the other makes the device manager walk a wrong
  pointer and fault -- which is exactly what happened, and why this is written down
  rather than left to the diff.
- **the series of transport frames, in progress**: a service that drives a device needs
  the *window* the machine's transports sit in, not one page of it -- the whole span from
  the untyped's base, so an address in the device tree is an offset into the window. The
  shape is settled and partly built (the survey retypes and keeps every page; the block's
  `Device` entry already carries the window's base in `number` and its length in
  `length`; the spawner maps one frame per page, page `i` being capability
  `device_frame + i`). Three things are known now and one is not:
  - **each page must ask for its own slot** even though they come out consecutive:
    reserving one and reaching past it leaves the allocator's cursor behind the slots in
    use, and the next allocation lands on a frame -- which the kernel reports as
    `seL4_DeleteFirst`, "the destination slot is occupied" (error 8, and that is the name
    to read it by).
  - **the window's first page is not a transport.** The untyped starts at `0x10000000`
    and the transports begin one page later, so a service handed the whole window must
    find its devices by *offset from the base* -- reading offset `0x08` of the first page
    faults (there is nothing there, which the survey already knew).
  - **what the supervisor actually receives, measured**: with the window handed over, a
    temporary print in the supervisor says

        supervisor: message badge 0, label 35, length 3

    which settles what the previous round could only guess at. This is **not a fault**: it
    is an *invocation* (a label and a length, and `seL4_MessageInfo_get_length` = 3 means
    it carried arguments), delivered to the shared fault endpoint with no badge. Badge 0
    is not a child -- a child's caps are badged with its own id -- so something invoked
    that endpoint, and the only thing that should ever *send* to it is the kernel
    delivering a fault. The suspect is therefore the capability that is being handed over:
    the window change assumes the frame capabilities the survey took are consecutive, so
    page `i` is `device_frame + i`, and if that arithmetic ever reaches a slot holding
    something else -- an endpoint, say -- the kernel is handed a page-map invocation it
    cannot make sense of. That is where the next attempt starts, and the first thing it
    should do is print the *slot numbers* it maps, because they should all be 4 KiB frame
    capabilities and the list will show the one that is not.
- **done**: the device manager spawns the virtio-blk driver itself, from the authority
  director delegates to it (the untyped, the ASID pool, its VSpace root, the initrd, and
  the device frames -- `specs/authority.md` records the pieces and the kernel rules they
  taught). It probes each granted frame for the virtio id in the device's registers,
  carves the driver's queue memory, mints it a log port, and waits for its ready before
  its own.
- **done**: the bus -> device -> service map is real. A static registry says which
  driver handles which compatible string -- and, on a virtio transport, which probed
  device id -- and the map is derived from the device tree at runtime, joined with the
  frames director granted: a device the tree describes but nobody granted is reported,
  not driven. Bound devices get instance names (`blk.virtio0`, not `blkdriver`), built
  when the join succeeds, and the map is sized by two walks of the tree so it grows
  with the machine. Adding a driver is adding a registry row.
- **done**: each driver's interrupt. IRQControl custody moved to the device manager
  (a copy derives to a null cap, so it *moved* -- specs/authority.md records the
  kernel lines); the handler is minted at the binding, paired with a notification and
  armed before the child starts, and the driver waits on it after each kick instead
  of polling the used ring. A driver that finds no pair polls -- virtio promises
  progress without one.
- **done**: the map is a port. `devmgr.registry` answers `count` and
  `describe`: a row is instance, compatible, binary, base, bytes, irq, window
  bits, and whether a driver is running (`libs/aegir-registry`). The partition
  manager asks before its walk and prints the map: eight virtio-mmio slots,
  one driven, seven reported unbound. The device manager serves it while still
  waiting for the partition manager's ready -- the child's supervision
  notification is bound to the serving thread, one receive sees both, and the
  call/signal convention above is what tells them apart.
- **done**: the map answers, and it also introduces. `open` on
  `devmgr.registry` takes a row's index and answers with one capability -- the
  bound driver's port, minted with the caller's badge so the driver sees the
  true caller, the shape `vfs.namespace`'s resolve already had. The device
  manager holds the unbadged original of every port it made for exactly this:
  an endpoint the spawner created may be minted again. Asking for an unbound
  row, or a row that does not exist, is the empty reply. This is how a client
  finds a spawned driver's port without a static edge in the manifest: walk
  `count`/`describe` to the instance name, `open` it. Landing it moved the
  port itself into the manifest (`devicemgr` owns `devmgr.registry`), because
  an endpoint made at runtime can never reach a service director starts -- and
  the owner half carries every right, because a mint keeps only what the
  source holds, while its callers carry the call mark, because the owner
  shares its receive with a supervision notification.
- **done**: the entropy source is a driver. virtio-rng (device id 4) was the
  device manager's own device -- the first proof that a service could read
  hardware, made when nothing else could. The proof served, and the transport
  joined the map like any other: a registry row (`id=4 prefix=rng`), and
  `rng.virtio0` is spawned from it. The driver is the smallest on the bus --
  one queue, and the device only ever writes into buffers the driver posts --
  which is what made it the second driver that measured the lib split. Its
  port serves one method, `read` (`libs/aegir-entropy`): one posted buffer
  per call, the answer is the bytes the device filled, up to what the
  envelope carries -- so its registry row declares `window=0`, and no shared
  window is carved for it. Its first consumer is the test bed, through the
  registry's `open`; the consumer the source is *for* is auth's nonce arc
  (specs/auth.md). **The lesson the second row taught the map**: a candidate
  match is on the compatible string, which every transport on the bus shares,
  so the first row that claims a transport is not necessarily the row for the
  device behind it -- the probe, the only read that says what is behind one,
  re-points the binding at the row the probed id names.
- **done**: the third driver has two queues, and its port can say "wait".
  virtio-input (device id 18, QEMU's `virtio-keyboard-device`) is what the
  `Queue` object's per-instance state was for: the *event* queue is primed
  with one device-writable buffer per descriptor -- the device only ever
  writes the eight-byte event (virtio 1.x, 5.8.6.1) into a posted buffer --
  and the *status* queue is set up and left idle. Its port
  (`libs/aegir-input`) serves two methods: `poll` answers whether an event
  waits, and `next` answers with the next event, one word -- and when none
  has arrived, the reply is *held*, not refused: the caller's reply
  capability is saved (`seL4_CNode_SaveCaller` -- a CNode invocation in
  this kernel's API, not a syscall) and the answer crosses when the
  interrupt lands. That is the supervisor-that-serves shape again: the irq
  notification is bound to the serving thread, one receive sees calls and
  signals, and a bare badge is the signal (a caller's badge is never bare
  -- the registry's `open` minted it). The queue's own size is the only
  bound on pending events, and a completed buffer is re-primed as it is
  consumed, so the driver holds no second queue of its own. The registry
  row is `id=18 prefix=kbd window=0` -- events ride in the envelope, like
  entropy. Acceptance presses a real key: the runner holds a QMP socket to
  the QEMU it started, issues `send-key a` when the test bed says it is
  waiting, and the check is EV_KEY 30 arriving down and then up through the
  held reply (scripts/run_target.py). Two lessons the landing taught: a
  bootstrap block's `DeviceCapability` entry keeps its slot in `reserved`,
  not `number` -- a free-slot scan that missed that saved the caller onto
  the frame cap ("Destination slot not empty", then a send through a
  page) -- and a third driver's images, queues and windows outgrew the
  spawner's 2 MiB delegation, which is 4 now (specs/authority.md's budget,
  not a capacity).
- **done**: the fourth driver draws -- two heads, and the first port whose
  window *is* the answer. virtio-gpu (device id 16) arrives twice on QEMU's
  command line (`gpu0`, `gpu1`); one registry row (`id=16 prefix=gpu
  memory=13 window=25`) serves both, the probe re-pointing each transport at
  it and the per-binding spawn loop starting one driver process per device --
  `gpu.virtio0` and `gpu.virtio1`, the instance model doing what it was built
  for. The driver runs the control queue only: `GET_DISPLAY_INFO` (honored,
  not hardcoded), `RESOURCE_CREATE_2D`, `RESOURCE_ATTACH_BACKING`,
  `SET_SCANOUT`, then `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH` per frame
  (virtio 1.x, 5.7). Its port (`libs/aegir-framebuffer`) serves `info`
  (width, height, format, stride, and physical size in millimetres -- zero
  when the device does not say), `flush`, and `set_mode`: a mode is refused
  only when `width*height*4` outgrows the window, because the window is the
  limit, not a mode list. Physical size comes from EDID
  (`VIRTIO_GPU_CMD_GET_EDID`, feature bit `VIRTIO_GPU_F_EDID` -- the first
  low feature bit any driver here negotiates; the handshake takes a
  wanted-mask parameter and the three older drivers ask for nothing);
  `GET_DISPLAY_INFO` itself carries pixels, never metrics. Acceptance is the
  screen, read from outside: the runner's QMP socket issues `screendump` per
  console (it works headless -- QEMU's display is `none`, and the console
  surface exists anyway), and the checks are the dumped dimensions and the
  band colors at them -- both heads at 1280x800 first, then `set_mode`
  shrinks `gpu.virtio0` to 1024x768 and grows it to 3840x2160, each time
  exactly one head changing, and 8192x8192 refused for outgrowing the window.
  Four decisions the sizing forced, one assumption the second head broke, and
  one note for the boards:
  - **the window rides as megapages.** 1280x800 at 32 bits a pixel is 1024
    4 KiB frames, and every CSpace in the system holds exactly 1024 caps
    (`kCNodeBits = 10`) -- per-page, the window path tops out near 256 KiB.
    So a window of 21 bits or more is carved as `seL4_RISCV_Mega_Page`
    frames (a real retype and map in this kernel), the spawn request carries
    a `window_page_bits` word, and the spawner maps frames of that size,
    aligning the window's address up to them; blk and the partition manager
    keep 4 KiB. A 32 MiB window is 16 caps, not 8192 -- and 32 MiB is what
    3840x2160x4 fits in, which is why the row says `window=25`. The window
    frames the partition manager pairs with block ports are gated to the
    `blk.` prefix: it pairs them 4 KiB at a time, and a scanout's mega pages
    are not the storage stack's to hand out.
  - **the delegation budget is per-service now.** A third bump of the shared
    constant would have hidden that devicemgr's appetite is not auth's: the
    manifest gains `delegate_mib` (absent = 4 MiB, the shared constant's last
    value), and devicemgr declares 68 -- two 32 MiB windows, the partition
    manager's megabyte, the filesystem image, and the drivers' images,
    queues and windows.
  - **the framebuffer is the shared window.** The block driver DMAs *from*
    its window; the gpu driver DMAs *from* its window to the screen --
    `ATTACH_BACKING` points at `window_physical`, and a client writes pixels
    into the mapped window and calls `flush`. Only devicemgr-spawned clients
    can map a window today, so the protocol ships ahead of its consumer; the
    test bed exercises `info` and `set_mode` without one.
  - **the grant was one device per id.** Director's grant to the device
    manager broke on the second head: a covered manifest child with a
    `device_id` was given the *first* bus device answering to it, so gpu1
    was in the tree with no frame granted ("the tree describes it, but no
    frame was granted"). The grant is now per *device* -- every transport
    the id answers to -- which costs the one-device drivers nothing and is
    what the instance model needed to be true.
  - **for physical boards this is the rehearsal, not the driver.** A board's
    display is its own compatible string -- `simple-framebuffer`, or a real
    display engine -- with its own row and driver, and none of the virtio
    command sequence leaves this app. What carries is the seam set: the
    megapage window (a display engine scans out of physically contiguous
    RAM by base address -- the same carve, and 4K is the same 16
    megapages), the port protocol (a board's `set_mode` may refuse --
    firmware-fixed modes -- and its EDID arrives over I2C, a `needs`
    edge), and physical millimetres in `info` (`0 = unknown` is the
    simple-framebuffer answer). Known limits, recorded rather than
    discovered: a fixed window has a top (8K wants `window=27`, and the
    elastic answer -- backing renegotiated with a memory server at mode
    change -- is a future arc), and `flush` is not vsynced (a board driver
    will want an irq-synced flip, driver-local when it comes).
  The runner's own lesson: the initial dump's cue is the *test bed's* line,
  not the drivers' markers -- the markers pass while the boot is still
  spawning, and a key pressed then is consumed by the keyboard check's own
  wait before the display check is listening.
- **done**: the third driver's kind is a family -- pointer devices join
  the keyboard, and the registry learns to tell them apart. virtio-mouse and
  virtio-tablet answer to the same virtio id 18 as the keyboard, so the probe
  that re-points a binding at the row the id names cannot place them: the id
  is the family, and the *kind* -- keys, relative motion, absolute position --
  lives in the config space's EV_BITS (virtio 1.x, 5.8.6.2), which the probe
  now reads when the rows for an id ask it to. The registry row gains an
  optional `evtype` key (`key`, `rel`, `abs`): three rows share `id=18`, the
  prefixes are `kbd`, `mouse`, and `tablet`, and one aegir-virtio-input
  process starts per device, as the second gpu head already proved. The
  driver itself is unchanged at the event layer -- events were always raw
  type/code/value in the envelope -- and learns to read its own EV_BITS and
  ABS_INFO, announcing its kind and its axes' ranges; libs/aegir-input gains
  the EV_REL/EV_ABS/BTN_* constants, no new protocol. QEMU grows two devices
  (`virtio-mouse-device,id=mouse0`, `virtio-tablet-device,id=tablet0`).
  Acceptance injects from outside: QMP `input-send-event` with no `device`
  argument -- the argument names a *console*, not an input device, and
  headless the events fall through to the unbound handlers, which sort by
  kind: absolute to the tablet, relative to the mouse, buttons to the mouse
  (QEMU bundles BTN into the relative handler's mask, so a tablet click is
  not injectable headless). Nothing scales the values on that path, so
  absolute and relative moves are both asserted exactly, buttons down and up
  through the held reply -- and every login's session opens `tablet.virtio0`
  and waits for one pointer event of its own (specs/auth.md's input path),
  the runner answering each session's cue as it prints.
  Two notes recorded with the decision: the probe's classification is the
  first device knowledge the binder holds beyond an id, contained to the rows
  that carry `evtype`; and a session that holds `devmgr.registry` can open
  any bound driver -- the per-device open policy is authority.md's open
  question, answered-for-now while the only sessions are smokes.
- **next**: the VFS and the `Initrd:` volume.

Devices are given to a driver the way everything else here is given: capabilities
for the device's register frames (retyped from the device's own untyped memory --
device frames cannot be retyped into anything else,
`kernel/src/object/untyped.c` refuses a non-page type on a device untyped) and an
interrupt handler (`seL4_IRQControl_Get`, `seL4_IRQHandler_SetNotification`,
`seL4_IRQHandler_Ack`) whose notification the driver waits on. A driver therefore
does not need to be told about the machine -- it needs to be told about *its*
device, and the map is what works out which is which.

## The storage stack, landed

The chain director → device manager → partition manager → filesystem service runs
end to end. The boot's own summary:

    spawned blk.virtio0 for virtio,mmio at 0x10007000, badge 257
    I am BD0: window of 64 KiB at 0x26000 (physical 0xffde0000)
    spawned partmgr, badge 264
    BD0Part0: sectors 2048..18431, "AEGIR" -- the system volume
    BD0Part1: sectors 18432..26623, "SECOND"
    BD0Part2: sectors 26624..32766, "SCRATCH"
    BD0Part3: sectors 32768..43007, "FAT16"
    spawned fat.BD0Part3, badge 512
    fat.BD0Part3: FAT16, 1 sectors per cluster, data starts at sector 287, writable
    spawned fat.BD0Part2, badge 513
    fat.BD0Part2: FAT32, 1 sectors per cluster, data starts at sector 632, writable
    spawned fat.BD0Part1, badge 514
    fat.BD0Part1: FAT32, 1 sectors per cluster, data starts at sector 758, writable
    fat.BD0Part1: SECOND.TXT says: a second volume, a second service, the same reader
    spawned fat.BD0Part0, badge 515
    fat.BD0Part0: FAT32, 1 sectors per cluster, data starts at sector 1010, writable
    fat.BD0Part0: AEGIR.TXT says: aegir read this file off a disk it enumerated itself

What was decided, and what it took:

- **The driver registry is data.** `manifests/drivers.registry`, a descriptor row
  per driver (`compatible=... id=... prefix=... bus=... binary=... memory=13
  window=16`), packed into the initrd and parsed by the device manager
  (libs/aegir-descriptor). Adding a driver is adding a row, not a recompile. The
  `window` field is the driver's to declare: how big a client's window its port
  serves through is, in bits.
- **A block device names itself.** The public namespace is the driver's business
  and nobody else's: the driver derives its unit from its instance name
  (`blk.virtio0` is unit 0) and answers `identify` with **BD0**. The device
  manager binds instances and never learns which of them are block devices.
- **The block port** (libs/aegir-block, v1 in full): `identify` writes the answer
  (name, sector count and size, the window's capacity) into the caller's window;
  `read` packs first-sector and count into one word (48 + 16 bits) and DMAs
  straight into the caller's window. Bulk data never crosses the message. **One
  window per client, not per device**: the endpoint serializes the DMAs, but it
  cannot stop one client's window from being written while that client is
  preempted between its call and its consumption, so each client reads through
  frames of its own. The window is mapped by the spawner, at spawn time, into
  the client that owns it -- a service cannot map into its own address space --
  and the bootstrap block carries it as a `SharedWindow` entry: virtual address,
  size, and the physical base a driver points virtqueue descriptors at. The
  driver learns which window belongs to which caller from the caller's clamp
  (`kMethodClamp` carries the window's physical base); the badge-0 caller, the
  device's manager, reads through the window it was itself started with.
- **Window frame caps multiply because a frame's first mapping pins its ASID into
  the capability** (specs/authority.md records the rule and the kernel lines).
  Every consumer gets its own cap set, minted before any mapping from a set that
  stays pristine -- and, now, its own frames: the partition manager carves a
  window per filesystem from its untyped, so no two readers share a window.
- **The device manager spawns the partition manager.** The alternative -- director
  starting it as a boot-set peer -- was rejected with the drivers: the director
  would have to learn the storage stack's insides. What the partition manager is
  handed is what bound: each block port's caller half (under the driver's
  instance name), its own window frame caps (one group per port, pages
  ascending), an untyped, the ASID pool, its VSpace root, the delegatable log,
  and the filesystem helper's image as a blob. It carves a filesystem's window
  from its untyped when it starts one, because a window shared by two clients is
  not safe under preemption (aegir/block.h).
- **The partition manager enumerates.** It maps each window, calls `identify`,
  and walks the GPT (protective MBR, header, entries). The entry table is read
  in window-sized runs, not one call, because the format's own minimum of 128
  entries is a floor and not a ceiling. A partition is named from
  the driver's name and the entry's index: **BD0Part0** -- the driver names, the
  manager enumerates.
- **Each partition gets a filesystem service with a range, not the device.** The
  range travels as a descriptor row (`first=2048 sectors=30687 name=BD0Part0`)
  written by the manager and parsed by the service; the service adds the offset
  to every read it makes. And it is enforced, not just read: the manager
  records the range with the driver (`kMethodClamp`, `libs/aegir-block`) before
  the child exists, together with the physical base of the window it carved for
  the child, the driver clamps every read by the caller's badge and DMAs into
  that badge's window -- badge 0 is the manager and the whole device, any other
  badge its recorded range, an unrecorded badge nothing -- and the manager
  proves each clamp holds by asking for sector 0 with the child's own badge and
  being refused.
- **The filesystem service's image travels as bytes** (`binary_image`), one
  helper at a time, because the whole initrd is 1.2 MiB and a copy per spawning
  service does not fit a service-sized delegation.
- **fs.fat reads FAT16 and FAT32**: BPB, the root
  directory, and a file's cluster chain -- the chain step is the one place
  the flavors differ (4-byte entries and one end-of-chain floor on FAT32,
  2-byte and another on FAT16, and the file walk branches on both). Long
  names travel too: a run of VFAT fragments before the 8.3 slot, tied to it
  by the name's checksum and decoded UTF-16LE to UTF-8, so a component
  matches the long name first and the 8.3 alias second (specs/fat.md). The
  test disk is built host-side without root (sgdisk writes the GPT, mtools
  fills each partition through `image@@offset`; scripts/make_disk.py):
  four partitions, AEGIR and SECOND and SCRATCH on FAT32 and the fourth
  FAT16 on purpose -- the write side's other flavor, exercised like the
  first. AEGIR and SECOND each carry one known file,
  because the second partition is the proof that the range grant works
  -- its BPB is nowhere near sector 0 -- and each file's content is the
  checksum, and SCRATCH starts empty because it is the write test's
  scratchpad: the test creates, writes, reads back, truncates, and lists
  there, under QEMU's `-snapshot` overlay so the disk image itself stays
  pristine.
- **fs.fat writes FAT16 and FAT32**: the volume protocol's handle side
  (`specs/vfs.md`) -- open/create/truncate, write at the cursor with the
  chain extended through the free-cluster scan, close; mkdir and remove,
  the tree growing and dying. A new cluster is
  zeroed before it joins a chain, because a multiuser system does not leak
  one file's old sectors into another; both FAT copies are written, and on
  FAT32 the FSInfo free count is marked unknown rather than maintained (the
  format allows it, and the scan never trusted it). The flavors differ
  where they always differ: 4-byte entries and a growable root chain on
  FAT32, 2-byte entries and a fixed root region on FAT16 -- a full FAT16
  root is full, and says so rather than growing. Writes are clamped by the same
  badge ranges as reads, so a partition's range stays its boundary. The
  writability of a volume is
  the partition manager's statement, carried to the filesystem in its
  descriptor row and to the VFS at registration, one source. The test
  proves the round trip byte-exact on both flavors: create, two writes
  across a cluster boundary, close, read back the recomputed pattern, list,
  truncate small again, remove -- plus the refusals (an existing name
  without `create`, the read-only initrd volume, a handle that is not one,
  a non-empty directory). `reap` and `unbind`, the teardown mechanisms
  (specs/vfs.md), are proven directly: the smoke session leaves one handle
  open on purpose, and the test reaps the badge, removes the file the
  handle was holding open, unbinds its Home:, and finds Sys: untouched.
- **Three latent limits broke on the way and are written down because they
  will not be the last.** The bootstrap block was capped at 512 bytes
  although it is mapped as a page, and a service with many grants (a window's
  frame per page) did not fit; it fills its page now. The virtqueue was
  single-shot -- the available ring always published slot 0 and the wait
  asked *nonzero* rather than *advanced* -- so the second read of a boot
  answered with the first request's used entry and a status of 0xff; the
  rings carry cursors now. And one shared window per device did not hold: the
  endpoint serializes the DMAs, not the consumers, so a partition manager that
  waited on the filesystem child it had just started resumed to a window full
  of that child's reads -- the second partition "did not exist" until the
  manager re-read the entry chunk after each spawn. The same hazard corrupted
  two filesystem clients once a second one existed, and the fix is the model
  above: a window per client, its physical base carried to the driver with the
  clamp.

Still open, in the order they arrive: a shell on the input path, and
resolve checks once volumes have an ownership model to check
against. Session reclaim landed (`specs/auth.md`): auth observes the exit
where it already waited for the ready, reaps the badge's handles on every
volume the namespace names, unbinds its aliases, and revokes the pool the
session's objects were retyped from -- sixteen logins run and reclaim in
one boot, which the 2 MiB spawn delegation could never have held at once.
The arc exposed one latent limit on the way, in the class of
limits-that-break: the alias table's arena assumed "a binding is small"
would cover ever-made rather than live-at-once, and the fifteenth session's
`Home:` was refused; unbound rows are reused now. Home volumes landed: the
system volume announces itself by partition type
GUID, the VFS aliases it `Sys:`, and a login makes and binds `Home:`
(`specs/auth.md`). The badge space is designed now
(`specs/authority.md`, Identity is a badge).
