# The VFS and the volumes

The VFS owns the namespace — volume names, path resolution, who may look up
what — and not the data (`specs/services.md`). Files live in the filesystems;
the VFS is the map. This spec fixes the map's protocol, how a capability
travels across it, and who serves the first volume.

## Volumes and paths

- A path is `Volume:rest`. The VFS resolves the part up to the first colon;
  **everything after the colon is the filesystem's to interpret**, including
  the Amiga parent convention — `/` is the parent, `//` two up — and case
  sensitivity, which is per filesystem (FAT matches case-insensitively; a
  native filesystem may not).
- **Volume names compare case-insensitively** (Amiga convention; FAT labels
  come up uppercase). `aegir:` and `AEGIR:` are the same volume.
- **Duplicate volume names get `_1`, `_2`, ... suffixes**, assigned by the VFS
  at registration — duplicates really only happen via filesystem labels, and
  the reply tells the registrant what name it actually got.
- The namespace is **open until `auth` lands**: resolve checks no badges yet.
  Every caller at this level is system authority. The badge chain below is
  built anyway, because clamping will need it.

## A capability travels over IPC

Resolve does not proxy; it **hands the client the filesystem's port
capability**. This is the first capability transfer over IPC in the system,
and the kernel's rules for it are what shapes the plumbing:

- The sender appends caps to the message; the kernel copies each into the
  receiver's receive slot (`transferCaps`,
  `kernel/src/kernel/thread.c:245-285`). The sender keeps its own cap.
- The transfer happens **only if the endpoint cap the sender invokes has the
  Grant right** (`doNormalTransfer`, `kernel/src/kernel/thread.c:212-218`) —
  otherwise the message goes through with no caps at all.
- A reply carrying caps needs Grant on the reply capability, which the kernel
  copies from the endpoint cap the **receiver** used in `seL4_Recv`
  (`kernel/manual/parts/ipc.tex`, "Calling and Replying"). So a port that
  carries caps needs Grant on **both** halves.
- The receiver names one receive slot with `seL4_SetCapReceivePath`
  (`kernel/libsel4/include/sel4/functions.h:98`); transferred caps land there
  and `extraCaps` in the returned tag says how many. A silent no-transfer is
  `extraCaps == 0` on a call that expected one — callers must check.
- The scratch receive slot is a system-wide convention:
  **`kSlotReceiveCap = 5`**, a fixed bootstrap slot like the fault endpoint's
  (`libs/aegir-bootstrap/include/aegir/bootstrap.h`). A received cap is moved
  out of it immediately — a second transfer onto an occupied slot fails.
- **Badged endpoint caps cannot be re-minted**, so a volume's caller half
  arrives at the VFS **unbadged**. On resolve the VFS mints a copy badged with
  the *caller's* badge: the filesystem then sees the true caller's identity,
  not the VFS's. That chain is the identity every later clamp relies on.

## The `vfs.namespace` protocol

Owned by the `vfs` service (boot-set order 3, before the partition manager, so
filesystems have somewhere to register — `specs/services.md`).

- **register** — words: the volume name, flags (read-only); one cap: the
  volume port's caller half, unbadged. Reply: the assigned name after `_N`
  dedup, or an error. Whoever spawns the filesystem registers for it, or the
  filesystem registers itself if it is a manifest service (see below).
- **resolve** — words: a path, `Volume:rest`. Reply: one cap, the volume port
  minted with the caller's badge, and where `rest` begins. Unknown volume is
  an error the caller can read (method numbers make that possible, per the
  versioning rule in `specs/services.md`).
- **count / describe** — the volumes, one row per describe: name, flags,
  whether a filesystem is bound. The registry pattern
  (`libs/aegir-registry`) applied to names.

The registration table **grows on demand**; there is no fixed volume count.

## The volume protocol

What a filesystem serves on its volume port. Version one is **stateless**:
no handles, no per-client state — the shape a multi-user system with many
concurrent readers wants, and FAT is read-only so there is nothing to
synchronize.

- **read** — words: path (after the colon), offset. Reply: data words inline
  in the envelope, a byte count, and an end-of-file flag.
- **list** — words: path, an index. Reply: one entry (name, size, kind), or
  end-of-directory. The cursor is the caller's index, the registry describe
  pattern again.

Inline data bounds a call to what the envelope carries
(`seL4_MsgMaxLength - 1` words). That is the right size for boot-time reads —
configuration, the user database, service images. The recorded scaling path,
when throughput matters: the client transfers a buffer capability at `open`,
the filesystem DMAs into it, and `read` replies with a count. It needs no
protocol change, only new methods — which is what method numbers are for.

A filesystem that serves from a shared window (the block layer's) copies the
data out into the reply inside the one call: the window's "content belongs to
the most recent call" caveat (`libs/aegir-block`) never reaches the volume's
clients.

## Who registers, who serves

- **`Initrd:` is the first volume**, served by a tiny **initrd** service
  (binary `aegir-fs-initrd`, manifest name `initrd` — `fs.*` is the
  partition manager's spawn class, and a manifest service must not wear it)
  — not by director, which never serves. Director spawns it with the
  archive's location and the owner half of `vol.initrd`; it mints the caller
  half itself, registers (`needs = vfs.namespace`, a manifest service can
  declare that), and serves read/list from the archive with libcpio, which
  is already vendored.
- **The partition manager registers on behalf of the filesystems it spawns.**
  A dynamically-spawned filesystem is not in the manifest and cannot declare
  `needs`, so the callback is a call to the partition manager's own port —
  `partman.partitions`, already named in the boot-set sketch. The manager
  creates each child's volume endpoint itself and keeps the caller half
  (unbadged, for the resolve mints); the filesystem **announces** the one
  thing the manager cannot know without asking — the volume's label, which
  lives in the filesystem's own structures — and the manager registers the
  label and the half it kept with `vfs.namespace`, answering with the name
  the volume actually got. A new filesystem type then implements announce +
  the volume protocol, and nothing about the VFS.
- The partition manager's caller cap on `vfs.namespace` travels director →
  device manager → partition manager, the way `devmgr.registry` already does:
  the manifest grants it to the partition manager, and the unbadged
  `spawn:`-prefixed copy flows down with the need-grant's rights.

## The supervisor that serves, without the demux

The partition manager's rhythm needs no badge mark at all. It walks every
partition table *first* and spawns afterwards, because a serving child uses
the same window frames the walk reads through — the walk's last read comes
before the first spawn. Then, per child: spawn, receive the one announce
(the child blocks in it until answered), register with the VFS, reply, and
only then wait for the child's ready. The announce receive sees nothing but
the one call it is waiting for, so there is nothing to tell apart. (The
`kCallMark` convention — calls carry the caller's badge with the top bit
set, signals arrive bare — remains the device manager's shape, where one
receive genuinely sees both, and lives in `aegir/ipc/port.h`.)

## The test service

`aegir-test` is the accumulating end-to-end test bed: a manifest service
(`restart = never`) whose output is part of the boot evidence. This arc's
tests: resolve `AEGIR:AEGIR.TXT` and `SECOND:SECOND.TXT` and read both back
byte for byte through the minted caps — the contents are the checksum,
`scripts/make_disk.py` put them there — list a root, and read the manifest
through `Initrd:` by its volume name. Later arcs add their tests here.
`hello` stays the spawn-smoke client.
