# auth: the user database and the login port

Status: specified, not yet implemented (2026-09).

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
| 4 | format version, `1` | 4 |
| 8 | user count | 4 |
| 12 | reserved, zero | 4 |
| 16 | rows, count of them | 80 each |

One row: `name[24]`, `account[24]`, `secret[32]` — each a NUL-terminated
string inside its field. The widths are format decisions, the way FAT's 8.3
is one: a name shares the system's name bound (`kNameMax`,
`aegir/nmspace.h`), an account names what it says, and a v1 plain secret
fits 32 bytes; the version field is how the format grows when any of those
stops being true. A secret in the v1 table is the plain word itself (see
Credentials, below).

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

The authoritative database on the root volume; challenge/response; a login
answer that carries what a session starts with (its badge, its pool, its
home volume); elevation through the sudo-like tool; attempt counting and
lockout. Each is in `specs/authority.md`'s open list, and each gets easier
when this slice stands.
