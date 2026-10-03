# libraries: resource libraries as services

Status: decided (2026-10).

Aegir's `libs/*` are static archives: compiled code a program links. That is one
kind of library. The other kind -- a **resource library**, one that owns
something a program cannot own for itself (a font engine, an image codec, a file
format) -- is wanted by many programs at once, and a private copy in each is the
waste a shared library exists to avoid.

This spec fixes how Aegir has them without a dynamic linker: **a resource
library is a service.** One process owns the resource and serves a port; a thin
static client library speaks that port; a program links the client and never
loads code.

## The decision

- **No dynamic linker.** Aegir does not load code into a running process: no
  shared object, no symbol resolution, no shared text, no `dlopen`. The reasons
  are already in the tree -- musl and libc++ are linked statically
  (`specs/cxx.md`) and a loader "wants its own arc with more than one client"
  (`specs/fonts.md`) -- and they are the right ones, not a shortcut to be paid
  down later. A *shared runtime* (one libc/libc++ mapped into every process) is
  not this spec and is not planned: it needs a symbol resolver, ELF TLS and
  per-process writable data, which is the Unix model and its own arc. The
  Amiga's libraries shared for free because it had no MMU -- one flat,
  unprotected address space. Aegir is seL4, per-process VSpaces, so sharing
  code is a deliberate mechanism, not a default.
- **A resource library is a service.** It owns the resource, holds the one
  copy, and serves a port. This is what `aegir-font` already is: FreeType lives
  in the `aegir-font` process and the toolkit draws through its client half
  (`specs/fonts.md` Phase 2). This spec names that pattern and gives it its
  idiom; it does not invent a mechanism.
- **The client half is a static library.** A program links `aegir-something`
  (the toolkit's `ServerFont` is the first) that marshals the port's protocol.
  No code is loaded at the client.
- **Eager or on demand is the composition question.** A dependency known at
  build time is a manifest `needs` and a port installed at spawn -- no
  `open_library` at all. A provider chosen at *run* time (which image codec for
  this file?) is the case `specs/direction.md` deferred until "a manager must
  start a component chosen at runtime"; `open_library` is that path, and
  `specs/datatypes.md` is its first caller.

## `open_library` and `close_library`

The interface is the Amiga's `OpenLibrary`/`CloseLibrary`; the mechanism is
Aegir's. `open_library` acquires a provider port for a name at run time;
`close_library` releases it.

- **`open_library(name, version)`** resolves `name` to a provider (below),
  starts it if it is not running, checks its protocol version against `version`,
  and returns a handle. A provider older than `version` is refused, as the Amiga
  refuses. The version is the port's own (`specs/services.md`: a protocol is
  versioned by method number), read from the provider's first reply, not guessed.
- **`close_library(handle)`** drops the reference. A **system** provider is
  resident once started (the Amiga default): close decrements a count and it
  keeps running, because a codec that reloads for every file pays the load cost
  every time. A **user** provider (below) is a session child and goes with the
  session. Stopping an idle system provider is a policy a manager may take
  later; the reclaim path exists (`mem.main`'s per-badge `release`,
  `specs/memory.md`) and is not this arc.
- **A handle is a capability.** A client never reaches a provider it was not
  handed a port to. The provider's port is minted for the caller
  (`specs/vfs.md`'s resolve mints the caller's badge), so the provider knows who
  is calling and a `close_library` that is not that caller's is refused, not
  trusted.

## Whose authority a provider runs with

A resource library is a service, but *whose* service depends on whose data it
handles, and that is what decides its authority. Two kinds:

- **A system resource library** owns system state and serves anyone -- the font
  service, or a system codec that decodes any user's file. It is a boot service
  (`specs/services.md`), runs with system authority, and is **resident once
  started**: a manager owns its memory and a session *leases* it, so it outlives
  the session that first wanted it.
- **A user resource library** handles a caller's data, and its code may be the
  caller's own -- a class from `Home:DataTypes`. It must run as the caller's
  **user class** -- a badge of the caller's user index, never a system one -- as
  a child of the session's launching machinery, reclaimed with the session. A
  badge is a user class and a serial, and only the class is authority: the serial
  is accounting (`specs/authority.md`), so "as the caller" is the user index
  shared by everyone in the session. It is **never** spawned by a system
  service; a system service may not launch out of a user's `Home:DataTypes` at
  all, or user-authored code would run as the system. That is not a policy to be
  careful about; it is the reason this distinction exists.

The rule is one sentence: **a provider runs with the authority of whoever owns
the data it handles, or less.** A system library is safe to share because it owns
the resource; a class that decodes *your* picture is yours and runs as you.

### The manager, per kind

- **A system library's manager is a system service.** It holds the
  name-to-binary mapping (the filesystem: *the list is the directory*, below),
  resolves names, starts providers under a spawn right it holds, and owns their
  memory. The device manager and the partition manager are the pattern
  (`specs/services.md`).
- **A user library has no system manager.** The session's launcher starts the
  provider as the caller's user class, the way it starts a command
  (`specs/launch.md`), and the caller talks to the port the launch produced. The
  `open_library` idiom is the client library's, over whatever started the
  provider; a session service that brokers a whole domain is the decided
  generalization (below). `specs/datatypes.md` is the first user resource
  library.

### The filesystem is the list

A library is addressed by its *name*, and its name is its filename, so a name
resolves through the caller's search path (below) to a binary. There is no
registry file: adding a library is dropping the binary in `LIBS:`, with no second
file to keep in step with the directory. A registry *is* the right shape where
the thing addressed is not a name -- a device's compatible string, a partition
type GUID -- which is why `drivers.registry` and `filesystems.registry` exist
(`specs/services.md`); a library is not that.

## `LIBS:` and the search path

`LIBS:` is the Amiga's library assign: an alias bound to `Sys:Libs`, per badge,
read as a `specs/namespace.md` union so a session may append its own `Home:Libs`
without touching the system volume.

`open_library` resolves a **bare** name by searching, in order:

1. the caller's **program directory** -- the directory the caller's own binary
   was loaded from, which the spawner records when it reads the image and gives
   the process beside its current directory (`specs/environment.md`); then
2. `LIBS:`.

The first step is the program's directory, not the current directory. They are
usually the same -- a program run from where its binary sits has both -- but they
are not always, and the one that matters is where the program's **own** libraries
live: the ones shipped next to its binary. A `cd` must not move a program's
private library out from under it, which is exactly what a search of the current
directory would do.

A path with a colon (`Sys:Libs/png.library`) is used as typed, with no search.
The search runs on the caller's badge, so a session's own `LIBS:` members are
visible to its `open_library` and to no other's.

## What this is not

- **A dynamic linker.** No code loading, no symbol resolution, no shared text,
  no shared object. A future arc may add a pager and file-backed mappings
  (`specs/clang-on-aegir.md`); a resource library stays a service through it, so
  this spec does not change. A shared *runtime* stays a separate decision.
- **A provider that never stops.** A *system* provider is resident once started;
  a *user* provider lives for the session (`Whose authority`, above). Reclaiming
  an idle system provider is available but not a feature here.
- **A plugin loader for untrusted code.** A provider is a service with exactly
  the authority whoever starts it grants it -- system authority for a system
  library, a badge of the caller's user class for a user library -- not arbitrary
  code in the caller's process. That is the point of the service shape.

## Open, for review

- **How a session starts its services.** The per-session broker is decided
  (`specs/datatypes.md`): a service holding the session's launcher kit that any
  of the session's processes may ask to open a class, started as the session's
  user class. What remains open is the general shape -- where auth puts a
  session's services beside the bureau and terminal, and whether that list is
  declared rather than coded into auth. That is `specs/auth.md`'s.
- **What `version` names.** The first cut is the port's protocol version,
  because a port already has one; a provider that also carries an Amiga-style
  `lib_Version` word can have both, with the protocol version checked first.
- **How a spawner starts a file it has never seen.** `open_library` resolves a
  name to a binary in the caller's search path; whether a caller may have any
  such binary started as a service, or a name needs a policy entry first, is the
  authority question, and it belongs with `specs/ownership.md`. For a user
  library the spawner is the session's launcher, so the check is the launcher's
  policy over what a badge may launch (`specs/launch.md`).
