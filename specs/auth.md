# auth: the user database and the login port

Status: first slice, sessions v1, homes, and session reclaim implemented
(2026-09) — the database, the packer, the service, the login check, a login
that starts a session under the user's badge, the home the session lands
in, and the teardown that takes an exited session back. Elevation remains
open; the input path landed with the pointer devices (2026-09) -- the
registry's `open`, at session distance, below. The greeter and the bureau
landed with the console arc (2026-09, `specs/console.md`).

`auth` owns two things: the record of who the users are, and the port that
answers "is this them" — `auth.login` (boot-set row 8, `specs/services.md`).
This spec fixes the first slice: the database's storage format and the login
protocol. Sessions, elevation, and what a login *answer* eventually carries
stay in `specs/authority.md`'s open list until this slice stands.

The model is authority.md's and does not change here: a user is a badge, a
name, and an account; the badge is the check; and multiuser is Aegir-level
records, not a kernel concept.

## The first slice

- A manifest service `auth` (`needs = log.main, vfs.namespace`, owns
  `auth.login`) reads the user database from `Initrd:` through the namespace
  — the first system consumer of the VFS, which is the point of doing it
  this way: the database is bytes on a volume, and everything that reads
  bytes on a volume goes through the map.
- It serves one method, `login`: a name and a secret in, an accept or a
  refuse out. No sessions are started, nothing is elevated, and the
  namespace stays open — resolve checks land with the session arc, when
  there is a user badge to check.

## The user database

A **binary table**, packed at build time and carried in the initrd under the
flat name `users.db`. auth parses the binary directly — no allocation, no
text format in the service. The human-edited source is a descriptor-row file
(`manifests/users`, the same row shape every other Aegir text file has,
`libs/aegir-descriptor`), packed by a script at build time: the text is for
people, the table is for the service.

The table, in full:

| offset | field | width |
| --- | --- | --- |
| 0 | magic, `"AUDB"` | 4 |
| 4 | format version, `2` | 4 |
| 8 | user count | 4 |
| 12 | reserved, zero | 4 |
| 16 | rows, count of them | 128 each |

One row: `name[24]`, `account[24]`, `secret[32]`, `home[48]` — each a
NUL-terminated string inside its field. The widths are format decisions,
the way FAT's 8.3 is one: a name shares the system's name bound
(`kNameMax`, `aegir/nmspace.h`), an account names what it says, a v1 plain
secret fits 32 bytes, and a home is a full `Volume:rest` path —
`Sys:Homes/` plus the longest name is 34, and the field rounds up with
room for an explicit one. The version field is how the format grows; v1
had no home, and v2 is the proof. The source row's `home=` says it,
defaulting to `Sys:Homes/<name>` — a default FAT can carry only while the
name is 8.3-clean, so a name that is not needs an explicit `home=`. A
secret in the v1 table is the plain word itself (see Credentials, below).

Lookup is by name, a linear scan: the table is small by definition (it is
the list of people who may log in), and a table that grows on demand is
about *count*, not row size.

**The initrd copy is readable by every boot service.** That is accepted, and
stated rather than stumbled into: while every caller is system authority the
secret is shared among superusers and nowhere else. The authoritative copy
moves to a volume only `auth` can read when resolve checks land
(`specs/services.md` already records that ordering: the initrd copy is the
bootstrap, the root volume's is the truth).

## Credentials, v1: the plain word over the port

The caller sends the secret itself, packed like any other string on the wire.
This is honest interim, chosen because the threat model that justifies more
does not exist yet: the only callers are boot services with system authority,
and a challenge/response exchange protects a secret from listeners on the
port — a set that is empty until user sessions hold caller caps. When
sessions exist, login becomes a challenge (`auth` issues a nonce, the caller
answers `H(secret, nonce)`, the secret never crosses), and the protocol's
method numbers are how that change arrives without breaking a caller. The
nonce's entropy is the rng driver's port — `rng.virtio0`, reached through
`devmgr.registry`'s `open`, and serving `read` today (`specs/services.md`);
the exchange lands when there are callers to protect the secret from.

## The `auth.login` protocol

Owned by `auth`. Strings travel in the namespace protocol's one shape
(`aegir/nmspace.h`'s `pack_string`); a method the port does not know is
answered by saying nothing, the system's versioning rule.

- `login = 1`. In: the name's words, then the secret's words. Answer: one
  word — **1 authenticated, 0 refused**. An unknown name, a wrong secret and
  a malformed call are the same 0: an oracle that says which half failed is
  a small gift to a guesser, and this port is not one.

The serve loop serializes attempts, which is all the rate shaping v1 has;
attempt counting belongs to the session arc, where there is someone to lock
out. Any caller may ask, while the namespace is open — a caller cap on
`auth.login` is a thing sessions are given, and sessions do not exist yet.

## The test

`aegir-test` gains three checks: the known user and secret the build packed
is accepted; the same name with a wrong secret is refused; an unknown name
is refused. The database's known entry is the checksum, the way
`AEGIR.TXT`'s contents are the disk's.

## Later, recorded rather than designed

The authoritative database on the root volume; challenge/response; attempt
counting and lockout; elevation through the sudo-like tool, when there is a
session to elevate from. Each gets easier when the slices above stand.

## Sessions, v1

A successful login starts a session. The decisions, taken 2026-09:

- **auth spawns the session itself**, with a delegated spawn kit -- untyped,
  an ASID pool, its VSpace root with an address window, the session binary
  as a blob, and `spawn:`-prefixed unbadged copies of the ports a session
  is given. This is the established pattern -- the device manager starts
  the drivers, the partition manager starts the filesystems, and the
  service that knows a login succeeded starts the session. It amends
  authority.md's "director, on request of an authenticated user": auth is
  the requester *and* the mechanism, and director stays static after boot.
- **The badge space is designed now.** Bit 63 is `kCallMark`, the
  call/signal mark a supervising server keeps. Bit 62 is the **user
  class**: a badge with it set belongs to a user, one with it clear to the
  system. A user badge is `bit62 | (user << 24) | serial`, where *user* is
  the row index in the user database -- free, stable within a build, and
  noted: reordering the source rows renumbers users, so the row order is
  part of the format's meaning -- and *serial* counts what the user runs.
  System badges stay low, as they already are. "Is this a user, and which"
  is a mask, not a table.
- **A login answers first, then spawns.** The reply says the credential was
  true; the session is its consequence, and a session that will not start
  is logged loudly rather than folded into the answer. While the spawn and
  the wait for the session's ready run, later logins queue at the endpoint
  -- the partition manager's synchronous rhythm, sufficient while sessions
  are short-lived; the supervisor-that-serves shape (one receive, calls
  marked, signals bare) lands when they are not.
- **The first session is a smoke, not a shell.** There is no input path --
  no keyboard, no serial input -- so an interactive session cannot exist
  yet. The session proves the authority shape instead: it runs with
  the user's badge and account, it logs, it reads through the VFS, it
  exits. The evidence is the logger's own lines: the logger prints the
  caller's badge, so a session's lines carry a bit-62 badge, and the
  identity chain is visible end to end. (Written when the filesystems were
  read-only; the home arc below is what a session was waiting for.)
- **The input path is the registry's `open`, at session distance.** A
  session's `needs` naming `devmgr.registry` travels the generic spawn-needs
  pipeline: the director hands auth an unbadged, mintable copy
  (`spawn:devmgr.registry`), and auth mints it per session with the session's
  badge -- with the call mark set, bit 63, because the device manager tells a
  call from a supervision signal by exactly that bit. The session then opens
  an input device by name (`tablet.virtio0` first) and waits on its held
  reply like any caller; acceptance injects one pointer event per login from
  outside (QMP `input-send-event`, specs/services.md's pointer bullet). What
  this is not: a focus model -- every session's open is equal, and routing
  "the" pointer to "the" foreground session is the console arc, not this one.
  And the grant is the whole registry, not one device: `open` carries no
  per-badge policy, so while the only sessions are smokes this is recorded as
  sufficient; authority.md's "may users hold device capabilities?" stays
  open. The console arc has superseded this for sessions (2026-09): the
  devices are the console's, the smoke's tablet open is gone, and a session's
  input arrives as windows' events (`specs/console.md`). What is written here
  remains true of the smoke-era mechanism, not of the bureau's.
- **A greeter asks; auth stays the database.** The GUI login prompt is
  auth's face, and auth spawns it -- the pattern the system already runs:
  the service that knows, starts it. Auth's `needs` gain `console.gui`, the
  director hands over the `spawn:`-prefixed copy, and once the database is
  read auth starts the greeter with the minted ports. The greeter is a pure
  UI process -- one window, two text fields, a button, an error line; it is
  the toolkit's first client now, and its form, look and console integration
  are `specs/trinket.md`.
  It calls `auth.login` like any caller: the credential check never leaves
  auth, and console is not a login caller. A refuse redraws the error line;
  an accept ends the greeter's part -- it welcomes the user, signals, and
  exits, and auth, waiting on that exit, reaps its badge (the console's
  `reap`: the window and the slice) before the session starts. The session
  binary is the caller's to choose: the bureau when the caller is the
  greeter, with `console.gui` in its `needs`; the smoke over the serial
  line.
- **Resolve stays open.** There is no volume-ownership model to check
  against, and a check without one would be an arbitrary rule, not a
  policy. Permission checks land with home volumes.
- **A login starts the bureau and the terminal.** Two session children, not
  one (specs/terminal.md, specs/shell.md): the bureau is the backdrop, and
  the terminal is the window above it that the command line runs in. Each
  has its own user badge, because the console carves one pixel slice per
  badge and refuses a second attach -- so a badge is a window's slice, and
  the terminal's serial is the bureau's plus one. The terminal is spawned
  second, so its window is created over the backdrop; it gets the namespace
  and the user's Home, as the bureau does. The session pool grew from 2 to
  16 MiB -- the two hosted images are near a megabyte each -- and the
  console's memory grant from 16 to 64 MiB, because the console retypes each
  client's slice from it, and a slice is the screen-bounded maximum the
  window may resize to.
- **Auth delegates the terminal's spawn kit; the terminal runs the
  commands.** A session runs user processes as ordinary use
  (`specs/authority.md`), and the terminal is the process that does: it
  holds the current directory and parses the line, so only it can build the
  request. Auth hands it a 4 MiB **command pool** -- the commands are retyped
  from it, and a command's exit is one revoke of the pool, so a long-lived
  terminal reclaims each command whole (specs/shell.md's Phase 4) -- a copy of
  auth's ASID pool, and the unbadged `spawn:log.main` and
  `spawn:vfs.namespace` copies its commands' own caps are minted from (an
  unbadged copy, because a badged endpoint cap cannot be minted again). The
  terminal's own toolkit untyped is 4 MiB: its heap holds the image of a
  command (near a megabyte) before the spawner copies it. It does not hand
  over the initrd: it is 5.7 MiB and does not fit a child, so the shell reads
  a command's bytes from `Initrd:` through the namespace and passes
  `Request.binary_image`. Auth's own stack grew to 32 KiB: the session start
  is stack-hungry (the home arc's path buffers, the spawn's frames), and the
  terminal's kit tipped an already-tight 8 KiB over.

## Session reclaim

An exited session is reclaimed by the spawner that started it: auth is the
caller who knows a session died. Drivers and filesystems never exit, so the
session is the first process whose objects outlive it -- and the first whose
reclaim is exercised. The decisions:

- **The exit is observed where the ready already is.** The serve loop's
  wait on the session's supervision notification returns when the session
  has finished -- the smoke signals it as its last act, so ready and exit
  are one signal while sessions are short-lived. The split -- ready early,
  exit late -- lands with the supervisor-that-serves shape, when sessions
  are not.
- **A session's memory is a pool auth retains.** The session's objects are
  retyped from one untyped carved out of auth's delegation and kept: auth
  holds the capability, so `seL4_CNode_Revoke` on it deletes everything the
  session was -- its CSpace, TCB, VSpace and frames, and with the CSpace
  the minted port copies it held (authority.md's retained-copy path,
  extended to sessions). The pool is then free whole and the next login
  reuses it: the wait serializes sessions, so one pool suffices, and the
  revoke is the only free the memory needs. The pool's size is a starting
  grant, measured from the account the session charges, not guessed.
- **Teardown order: reap, unbind, revoke.** The badge's handles go first --
  one `reap` per volume the namespace names, walked through
  `count`/`describe`/`resolve`, because a handle is a filesystem's row and
  not a kernel object -- then the badge's aliases with `unbind`, then the
  revoke. The mechanisms were landed and tested ahead of their caller
  (specs/vfs.md); this is the caller they were waiting for. The console
  arc's `reap` joins the same order where windows must go: the greeter's
  badge is reaped through `console.gui` before its login's session starts
  (`specs/console.md`). A session's own slice is deliberately NOT reaped --
  the bureau's backdrop is the session's visible remainder, and taking it
  down is the re-login arc's.
- **Slots come back too.** The capabilities a spawn puts in auth's own
  CSpace die with the revoke, and the slot cursor returns to the mark the
  login took (`slot_mark`/`slot_release`, libs/aegir-mem) -- valid exactly
  because the revoke emptied the range. Without it the drain only moves
  from the untyped to the CSpace, the same leak in a different hat.
- **The test proves the drain is closed.** Logins past what the grant could
  hold unreclaimed all succeed, and the handle a session leaves open is
  already gone when the next caller asks -- the test no longer reaps it
  itself.

## Homes

A login gives the session somewhere to be. The decisions:

- **The user row carries the home**: the `home[48]` field above, the path
  the session's `Home:` stands for, `Sys:Homes/<name>` by default. The
  database is the record of who the users are; where they live is part of
  who they are.
- **auth ensures the home, makes it the user's, then binds, then spawns.**
  On a successful login: one `mkdir` of the row's home path through the
  namespace (the volume protocol's `mmd` shape makes the whole chain one
  call); `Owner` and `Protect` on it so the directory is the user's and
  owner-only; one `mount` of a view over it owned by the session's badge;
  one `bind` of the session's badge to `Home` → that view
  (`specs/vfs.md`'s aliases, `specs/ownership.md`); then the spawn as
  before. The order is the point: the session never sees a `Home:` that does
  not resolve, and never one that is not its own.
- **A home that will not create does not stop the login.** The mkdir's
  failure is logged, the bind is made anyway, and the failure surfaces
  where it belongs: the session's first write into it is refused by the
  filesystem. A read-only `Sys:` is a fact, not a reason to keep a user
  out.
- **The home also carries the session's scripts.** Once the home exists,
  auth makes `Home:S` the user's (the same `mkdir`/`Owner`/`Protect` as the
  environment archive, `specs/environment.md`) and binds the session's badge
  to `S:` → `Home:S` (`specs/shell.md`). It is where a user's
  `Shell-Startup` override lives. The system's `Sys:S` is a separate name,
  never unioned: the system's scripts do not appear in the user's `S:`, and
  editing them needs elevation.
- **The smoke proves it end to end.** The session resolves
  `Home:WELCOME.TXT`, creates it, writes, closes, reads back — and
  `aegir-test`, under its own system badge, reads the same bytes back
  through `Sys:Homes/rroland/WELCOME.TXT`. Two badges, two names, one file:
  the alias is the namespace's, not the session's imagination.
