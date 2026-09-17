# auth: the user database and the login port

Status: first slice and sessions v1 implemented (2026-09) — the database,
the packer, the service, the login check, and a login that starts a session
under the user's badge. Elevation, an input path, and session reclaim
remain open.

`auth` owns two things: the record of who the users are, and the port that
answers "is this them" — `auth.login` (boot-set row 7, `specs/services.md`).
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
method numbers are how that change arrives without breaking a caller.

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
- **Resolve stays open.** There is no volume-ownership model to check
  against, and a check without one would be an arbitrary rule, not a
  policy. Permission checks land with home volumes.

Known gap, stated rather than stumbled into: **an exited session is not
reclaimed.** Drivers and filesystems never exit, so this is the first
process whose objects outlive it; repeated logins drain the spawn untyped
until reclaim (authority.md's retained-copy path) extends to sessions. The
bound is the grant, and reaching it is a loud refusal, never a quiet one.

## Homes

A login gives the session somewhere to be. The decisions:

- **The user row carries the home**: the `home[48]` field above, the path
  the session's `Home:` stands for, `Sys:Homes/<name>` by default. The
  database is the record of who the users are; where they live is part of
  who they are.
- **auth ensures the home exists, then binds, then spawns.** On a
  successful login: one `mkdir` of the row's home path through the
  namespace (the volume protocol's `mmd` shape makes the whole chain one
  call), one `bind` of the session's badge to `Home` → that path
  (`specs/vfs.md`'s aliases), then the spawn as before. The order is the
  point: the session never sees a `Home:` that does not resolve.
- **A home that will not create does not stop the login.** The mkdir's
  failure is logged, the bind is made anyway, and the failure surfaces
  where it belongs: the session's first write into it is refused by the
  filesystem. A read-only `Sys:` is a fact, not a reason to keep a user
  out.
- **The smoke proves it end to end.** The session resolves
  `Home:WELCOME.TXT`, creates it, writes, closes, reads back — and
  `aegir-test`, under its own system badge, reads the same bytes back
  through `Sys:Homes/rroland/WELCOME.TXT`. Two badges, two names, one file:
  the alias is the namespace's, not the session's imagination.
