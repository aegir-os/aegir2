# What a session is made of

Status: decided (2026-10). `specs/auth.md` keeps the login port, the user
database, the homes and the reclaim; this is the composition a successful login
builds. The manifest reader and the `datatypes` broker are not built yet; this
is the design they build to (see *Phases*).

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
  (`specs/shell.md`). A user copies the system's and edits it to change their
  session.
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
- **The boot manifest's `[session.*]` entries are superseded.** The boot
  manifest composes system services (`specs/director.md`); a session's services
  live here. `manifests/services.manifest`'s `[session.*]` sections are removed
  as the reader lands.

The shipped default (`Sys:S/session.manifest`) declares the bureau and a
terminal. The terminal is **convenience while there is no desktop**, not
structure: when the desktop with icons lands, the default's terminal section
goes and nothing in auth changes -- that is the point of declaring it.

## Starting it

auth parses the manifest before spawning anything, then starts the launcher and
each declared service in order, as the session's user class. A service lives for
the session; reclaim takes it with the session (`specs/auth.md`). Reclaiming an
idle service is a later feature, not a rule here.

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
**once** (not per shell), through the launcher, so a user can start extra
programs beyond the manifest -- the manifest is structure, User-Startup the
user's additions. Recorded here so the manifest and the script are one design;
`specs/shell.md`'s `S:Shell-Startup` is the per-shell sibling.

## Phases and acceptance

1. **The manifest reader.** auth reads `Sys:S/session.manifest` and starts the
   bureau and terminal from it, replacing the hardcoded shape (`aegir-auth`'s
   `start_session`). The acceptance is the existing login arc: same screens,
   same cues, with the composition now data. Malformed and absent take their
   recovery paths.
2. **The datatypes broker.** The first *new* service the manifest declares: owns
   `datatypes.main`, needs `vfs.namespace` and `launch.session`, starts classes
   as the session's user class (`specs/datatypes.md`, `specs/libraries.md`). The
   client asks it when present and falls back to the direct serve-launch when
   not.
3. **User-Startup.** Run once at session start, after the services are up.
