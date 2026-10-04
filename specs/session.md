# What a session is made of

Status: decided (2026-10). `specs/auth.md` keeps the login port, the user
database, the homes and the reclaim; this is the composition a successful login
builds. The manifest reader, the `datatypes` broker and `User-Startup` are built
(see *Phases*).

## A session is one user class, a range of serials, and a launcher

auth makes a session on a successful login (`specs/auth.md`). It is:

- **one user class.** Every program in the session carries a badge with the same
  user index -- the caller's `user class`, never a system one
  (`specs/authority.md`, `specs/libraries.md`). The badge's serial is accounting,
  not authority: it is what a program's memory is charged to and what reclaim
  returns.
- **a range of serials.** auth mints the session child, its terminal and its
  launcher from a stride, and hands the rest to the launcher, which mints a
  serial per program (`specs/launch.md`'s `AEGIR_BADGE_RANGE`).
- **a namespace**, bound for the session's badge: `Home:`, `ENV:`, `S:`, `C:`,
  `DataTypes:`, `LIBS:` (`specs/namespace.md`, `specs/dos.md`). Every program
  resolves through it, so a session's `Home:` is the user's and `C:` is the
  command set.
- **an account**, the user's, so limits and memory fall on the user
  (`specs/limits.md`, `specs/memory.md`).
- **a launcher** (`specs/launch.md`), which auth makes and hands the owner half
  of `launch.session`. It is the session's way to start a program and is not
  optional and not declared: a session with no launcher could start nothing.

## The session manifest

A session's *services* are data, not code: a manifest in the boot manifest's
format (`libs/freestanding/aegir-manifest`, `specs/services.md`), read by auth.

- **Location.** The system ships `Sys:S/session.manifest`; the user's is
  `Home:S/session.manifest`. auth reads the user's if it exists, else the
  system's -- the user-first/system-fallback rule `S:Shell-Startup` already uses
  (`specs/shell.md`). A user *may* copy the system's and edit it, but this is an
  **expert** file: the ordinary way to change a session is `Home:S/User-Startup`
  (below), which starts programs without redefining what a session *is*. The
  manifest is here for power users, and for the system to declare the session's
  own shape as data.
- **Format.** `[name]` sections with the boot manifest's keys. The ones that
  matter here: `binary`, `authority`, `account`, `needs`, `owns`, `maps`,
  `memory_kib`. Unknown keys and sections are errors, not skips
  (`aegir/manifest.h`).
- **`authority` is always `user`.** A session manifest may not declare a system
  service: that is the hole the user/system split exists to close
  (`specs/libraries.md`). A `system` authority here is a parse error.
- **`needs` is a closed vocabulary** auth can satisfy, each name resolved to a
  capability auth already holds: `log.main`, `vfs.namespace`, `console.gui`,
  `font.main`, `clock.main`, `timer.main`, `mem.main`, `launch.session`,
  `bureau.menu`, `datatypes.main`. An unknown need is a parse error -- the
  manifest names what a session has, and auth never guesses.
- **The launcher is not a section.** auth makes it before the manifest's
  services, so the manifest declares what rides *on top of* the session's floor.
- **The boot manifest declares the session's *authority*, one entry,
  `[session.authority]`.** A session's services are this manifest's; but auth
  can only satisfy a `needs` name with a capability it holds, and the unbadged
  sources (`spawn:...`) it delegates are what *director* hands it, derived from
  the entries its `[auth] spawns = session.*` covers
  (`aegir-director/src/services.cc:453-484`: "an unbadged copy of every port its
  children need"). The old `[session.smoke]`/`[session.bureau]`/
  `[session.terminal]` entries served that; they are replaced by one entry that
  names the *union* a session service may ask for -- `log.main`,
  `vfs.namespace`, `console.gui`, `font.main`, `clock.main`, `timer.main`,
  `mem.main` -- and declares no service. auth does not spawn it; it is the
  vocabulary, not the composition. `launch.session` and `bureau.menu` are absent
  because auth makes those ports itself. Deleting the old entries without this
  replacement drops `spawn:clock.main` and `spawn:timer.main`, and the terminal
  it starts then has no clock or timer.

The shipped default (`Sys:S/session.manifest`) declares the bureau and a
terminal. The terminal is **convenience while there is no desktop**, not
structure: when the desktop with icons lands, the default's terminal section
goes and nothing in auth changes -- that is the point of declaring it.

## Starting it

auth parses the manifest before spawning anything, then starts the launcher and
each declared service in order, as the session's user class. Each runs under its
own process badge, but receives a namespace badged for the **session** -- the one
set of aliases every program of the session resolves through -- so a service's
file reads are the session's and one alias set serves them all, rather than one
per service. A service lives for the session; reclaim takes it with the session
(`specs/auth.md`). Reclaiming an idle service is a later feature, not a rule
here.

## When the manifest is wrong

- **Absent.** No `Home:` and no `Sys:` session manifest: auth starts the
  built-in minimum -- a terminal and the launcher -- so a login cannot come up
  empty.
- **Malformed.** auth announces it **loudly**, on the boot serial and the
  logger, with the parser's line and reason -- `auth: session.manifest: line 7:
  unknown key 'binry'` -- and still starts a terminal and the launcher, so the
  user has a way to edit the file and recover. A silent skip is exactly how a
  typo becomes a session that runs the wrong thing (`aegir/manifest.h`'s rule);
  the loud failure is the reason the parser refuses to be forgiving.
- **Well-formed but thin.** A manifest that omits the terminal is the user's
  session; only the launcher is guaranteed. The desktop arc makes this the
  normal case.

## User-Startup

Once the manifest's services are up, the session runs `Home:S/User-Startup`
**once** (not per shell), so a user can start extra programs beyond the
manifest -- the manifest is structure, User-Startup the user's additions. It is
the session's *own* shell that runs it: auth marks the shell host it starts for
the session's composition with the argument `--session`, and a shell that sees
it runs the file before `Shell-Startup`. A nested shell is started by the
launcher and carries no such argument, so it runs only `Shell-Startup` -- which
is what makes it once per session and not once per shell. An argument, not an
environment entry: a child inherits its parent's environment, so an env mark
would reach a nested shell and run User-Startup again. The commands it starts go
through the launcher, like every other command. `specs/shell.md`'s
`S:Shell-Startup` is the per-shell sibling; when the desktop lands it is the
session's shell and runs User-Startup the same way.

## Phases and acceptance

1. **auth makes the session's ports.** Landed. auth makes `bureau.menu` itself,
   from the session's own allocator, the way it already makes `launch.session`.
   The boot manifest's `[session.*]` services are gone, replaced by one
   `[session.authority]` entry that declares the vocabulary auth must hold the
   sources for; the composition is the session manifest's alone.
2. **The manifest reader.** Landed. auth reads `Home:S`/`Sys:S/session.manifest`,
   parses it, and starts the bureau (windowed) and terminal (launcher-shaped)
   from it, replacing the hardcoded shape (`aegir-auth`'s `start_session`). A
   malformed file is announced with the parser's line and reason and leaves the
   built-in terminal; an absent one does the same quietly. `needs` is resolved
   through a closed vocabulary, `owns` through what auth can make; the launcher
   is not a section and is still started by auth.
3. **The datatypes broker.** Landed. The first *new* service the manifest
   declares: `aegir-datatypes-broker` owns `datatypes.main`, needs
   `vfs.namespace` and `launch.session`, and starts classes as the session's
   user class (`specs/datatypes.md`, `specs/libraries.md`). auth makes the
   broker port from the session's own allocator and hands the launcher its
   source, so the client asks the broker when present and falls back to the
   direct serve-launch when not.
4. **User-Startup.** Landed. `Home:S/User-Startup` runs once by the session's
   own shell, before `Shell-Startup`; a nested shell does not. The
   `Sys:Homes/<name>` the auth row names ships with it (the disk's), and the
   acceptance cues on the shell's own line so a per-shell run would be visible.
