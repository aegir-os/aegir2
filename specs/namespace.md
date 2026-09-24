# The namespace and the union

Status: decided (2026-09). This spec extends `specs/vfs.md`'s namespace — the
map of volume names and aliases — with the **union**: a name that stands for an
ordered list of paths, read as one directory.

It is not a new idea. The Amiga's multi-assign (`Assign LIBS: Work:libs ADD`)
already makes a name stand for an ordered list of directories searched in
order, and Plan 9's `bind` is the same shape with a namespace behind it. What
this spec fixes is that the union is a **filesystem the VFS serves**, not a
search list each client resolves: a read or a listing of a union returns the
merged view, so `aegir::filesystem` and `std::filesystem` both see every
member's entries and neither reconstructs the merge.

## The decisions

- **A binding is an ordered list, not a path.** `vfs.namespace`'s `bind`
  already stands a name for one path (`specs/vfs.md`'s aliases); a name may
  stand for a list, and the list's order is the search order. A one-member list
  is the alias it already had.
- **A union reads and lists as one directory.** A read of `Name:rest` walks the
  members in order and stops at the first that has `rest`. A listing of
  `Name:rest` returns every member's entries for `rest`, merged, with a name an
  earlier member has winning — so a name in two members appears once. This is
  what makes `directory_iterator("ENV:")` list the whole union; the merge is
  the VFS's, not a caller's.
- **A create goes to the designated member.** One member is the **create
  target** (Plan 9's `-c`, the Amiga's first-writable): `create`, `mkdir` and a
  write that makes a new name land there, and a create the target refuses is
  refused — there is no fall-through to another member, because a write must
  not land somewhere the binder did not choose.
- **A remove goes to the first member that has the name.** Removing a name a
  union shadows removes the member that actually holds it, which is the same
  rule a read uses; the member's own authority decides whether it may.
- **Precedence is a binding choice.** Appending puts a member after the current
  list, prepending puts it before, and the default replaces the binding. So one
  mechanism serves a search path (`LIBS:`, append), an override (`ENV:`, the
  user's archive before the system's), and an overlay of a read-only volume
  with a writable scratch.
- **The union is the VFS's to serve.** The members are volumes the namespace
  resolved; the VFS holds their capabilities and presents the merged directory.
  A client resolves the name once and gets the union's volume capability; the
  volume protocol's read and list carry the merge, and the create target is
  where its open and mkdir go. (The alternative — a search-list alias whose
  members every client tries — puts the merge in every client, and is the thing
  this avoids.)
- **Bindings are per-badge, as the aliases are.** The badge names whose
  namespace the binding is in (`specs/vfs.md`); a session's processes share the
  session's badge and so share its `ENV:`. A finer, per-process namespace —
  binds private to one process and inherited by its children, Plan 9's `rfork`
  shape — is a later extension; this spec's binding table is what it would key
  differently, so it is not foreclosed.
- **The ownership model decides who may bind what.** While the namespace is
  open (`specs/vfs.md`), nothing checks the badge a binder binds; the check
  lands with the ownership model (`specs/ownership.md`), which gates `bind`
  through the owner-aware resolve it already performs. A union whose create
  target is a system
  archive is not a privilege escalation by itself: the binding says only where
  a write is attempted, and the archive's own authority decides whether it may
  land.

## The shape

### `bind`, grown

`vfs.namespace`'s `bind` keeps its shape — the badge, the name, the path — and
grows a flags word: append, prepend, or replace (the default), and whether this
member is the create target. A second `bind` of a name with append or prepend
adds a member; the default replaces the list, which is the one-member alias
`specs/vfs.md` already describes. `unbind` drops the badge's bindings as it
does today, a union with them.

### The union directory

The VFS keeps, per binding, the members' volume capabilities and their order,
and serves the union through the same volume protocol every other directory
uses:

- **list** — every member's `list` for the path, in order, merged; a name an
  earlier member returned is not returned again.
- **read** — the first member that has the path.
- **open** with `create`, and **mkdir** — the create target.
- **remove** — the first member that has the path.

The members are ordinary volumes; the union does not need them to know they are
in one.

### `ENV:` is one instance

`specs/environment.md`'s persistent store is the first use: a session's `ENV:`
is the union of `Home:Prefs/Env-Archive` (first, and the create target) and
`Sys:Prefs/Env-Archive` (the base); a system-authority process's create target
is the system archive. Nothing about that is special to the union — it is a
binding, and the archives are ordinary directories.

## Implementation

Six slices, each its own commit, because the wire changes and the serving
cannot land half-way (all six are landed):

1. **The `bind` wire** (`libs/aegir-namespace`). `bind` grows a flags word --
   append, prepend, or replace (the default), and create-target -- so its words
   become `{badge, flags, name, path}`. The one existing caller, auth's `Home:`,
   sends the replace flag. `unbind` is unchanged.
2. **The data model** (`apps/aegir-vfs`). A `Binding` keeps the flags and an
   *ordered list* of member paths, not one target. `answer_bind` appends,
   prepends or replaces; `unbind` drops the list. `find_binding` and the
   single-member case are unchanged.
3. **The union volume** -- the substantial piece. The VFS creates a port and
   **serves the volume protocol on it** (`aegir/volume.h`: read, list, open,
   write, close, mkdir, remove), forwarding to the members: list merges (a name
   an earlier member has wins), read and remove take the first member that has
   the path, open-with-create and mkdir go to the create target.

   The port is the **namespace endpoint itself**, not a new one: a single
   thread cannot `seL4_Recv` on two endpoints, and polling a second one is a
   spin, so the union is served on the endpoint the VFS already receives on.
   `resolve` of a union returns that endpoint's capability, minted with a
   badge that encodes the **union id**; a plain namespace capability carries no
   union id, so the one receive tells a union call from a namespace call and
   knows which union. The union then speaks the **volume protocol** on that
   capability -- read and list carry the merge, so a client that resolved a
   name calls them exactly as it calls a volume's -- and the badge's mark is
   what lets the VFS tell the two meanings of method 1 apart (a namespace
   `register`, a union's `read`).

   The members are resolved when they are **bound**, not per call: `bind` turns
   each path into the volume it names and the volume-relative rest, and the
   binding keeps those (the section above), so a member can be a per-badge
   alias (`ENV:`'s `Home:Prefs/Env-Archive`) without a later call knowing the
   badge. The write side is landed on the **binder's badge** -- the same badge
   read and list already forward on, and the system's when the binding is
   global, because the everyone-badge is a sentinel and a member would read it
   as a user (`aegir-vfs`'s `member_badge`). A member therefore scopes a handle
   to the binder, not the union cap's caller; the caller's own identity is
   still not in the union cap's badge (the union id is), and a client cannot
   learn its badge today, so the true-caller identity remains the write side's
   own piece. A union handle is the union's own serial over the member's, so
   write and close reach the member the open did; `open` with `create` and
   `mkdir` go to the create target (the member marked `kBindCreate`, else the
   first -- the Amiga's first-writable), and `open` of an existing name and
   `remove` go to the first member that has the path.

   It also widens a right: minting the union cap is a mint *of the namespace
   endpoint*, and a mint keeps only what the source holds, so the VFS's owner
   half must carry Write and GrantReply for the client's cap to be callable.
   `apps/aegir-director/src/ports.cc`'s `rights_for("vfs.namespace").owner`
   grows from `Grant+Read` to all rights for that reason. Nothing else about
   the port graph changes.
4. **`resolve` of a union** returns the union endpoint's capability, minted
   with the **union id** in its badge, and the rest -- the shape a volume's
   resolve answers in, so a client sees no difference. The VFS is then a
   filesystem whose members are filesystems.
5. **The acceptance**: the test bed binds two directories on `AEGIR:` and
   `SCRATCH:` into one name and exercises read (first member wins), list
   (merged, shared name once), mkdir/create (create target only), and remove
   (the member that holds it).
6. **The enumeration**: three more methods on `vfs.namespace` --
   `bindcount`, `binddescribe`, `bindmember` -- expose the bindings, which a
   shell otherwise cannot see. A binding's row carries its name, flags, union
   id and member count; a member's row carries the volume it pins, its flags,
   and its rest. This is what lets a DOS command set a union up and remove a
   volume from one, and the groundwork for naming a filesystem for a mount.

`ENV:` is then a binding, not code: the archives are ordinary directories and
`ENV:` is their union (specs/environment.md). The startup that reads it and the
DOS toolset that writes it are the storage arc's.

## What this is not

Per-process namespaces (a later extension, above); a union of files (only
directories union — a name's members must be directories); the ownership check
on `bind` (the ownership model's arc); a filesystem that unions across *types*
(the members are volumes, whatever serves them).

## Acceptance

Landed in the test bed (`apps/freestanding/aegir-test`). It binds two
directories on two volumes — `AEGIR:` and `SCRATCH:` — into one name: a read of
a file both hold returns the first member's bytes; a listing returns both
members' entries with the shared name once; a `mkdir` and a create land in the
create target (`SCRATCH:`, the member marked `kBindCreate`) and not the first
member; a remove takes the first member that holds the name. `ENV:` is read and
listed once the storage arc has the archives, and is not this arc's test.
