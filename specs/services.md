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
- **Who registers `Initrd:`** — proposal: director, the moment the VFS is up.
- **Hot-plug** (a device appearing later) and **device removal** are unmodelled;
  both end up as registry updates plus spawn/stop requests.
- **What a session is made of** — the services an interactive user actually gets
  — is a later spec.
