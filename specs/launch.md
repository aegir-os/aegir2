# launch: starting a program from a session

Status: decided (2026-09). The request shape, the kinds and the authority are
below; `Run` (kind 1, without waiting) has landed, and the `aegir::launch` API
with the per-session launcher is the next work (specs/memory.md Phase 5).

Aegir's processes are not forked: a **spawner** creates a child out of
authority it was delegated (specs/authority.md). That is the mechanism, and it
should not be the interface. A program starts another the way a program always
has -- `fork`/`exec`, or `posix_spawn` -- and **inherits its context**: current
directory, environment, path, prompt and stack. The Terminal starting a
MultiView, a desktop icon starting an editor, a Bureau menu's Execute starting
a Shell, and `NEWSHELL` starting another Terminal are all the same act. This
spec is that act: what a launch carries, what kinds of program there are, and
who is allowed to fulfill one.

## The interface is a library, not a service call

The C++ face is `aegir::launch`; the C runtime underneath it carries the
process primitives (`posix_spawn`-shaped: program, arguments, environment,
file actions), so a developer chooses the C++ call or the C one. Neither ever
serializes the caller's context by hand: the runtime fills the request from the
process's own state -- `environ`, the current directory it is tracking, its
path, its stack request -- exactly as `fork` hands a child the parent's. Inherit
is the default, because the caller *is* the counterpart of the parent.

    // C++
    aegir::launch::Program child{"aegir-multiview", {"Work:Pic.iff"}};
    aegir::launch::Window window{"CON:64/64/640/400/MultiView"};
    aegir::launch::spawn(child, aegir::launch::Kind::Windowed, window);

    /* C */
    spawnve(file, argv, environ, SPAWN_WINDOWED, &window_spec);

`aegir::launch` adds no policy of its own: it builds the request and calls the
launcher (below). A program with no launcher capability gets a refusal, the
same way a `fork` in a system that caps processes does.

## What a launch carries

A launch request is the spawn `Request` (specs/services.md) plus the things a
*session* launch needs and a boot spawn does not:

- **the program and its arguments** -- `argv[0]` the program's name, the rest
  as written (the shell substitutes and groups before it gets here,
  specs/shell.md);
- **the launcher's context**: environment (`NAME=VALUE`), current directory,
  path, and stack request. The runtime reads these from the caller, so the
  inheritance the manual promises ("the new window has the same current
  directory, prompt string, path, local environment variables, and stack
  size") is a property of the call, not of plumbing the caller writes;
- **the kind**, below;
- for a windowed program, **the window specification**: the Amiga
  `CON:x/y/width/height/title/options`, parsed by the launcher into the
  window's rectangle, title and flags (`/CLOSE`, `/AUTO`, `/INACTIVE`, ...);
- **who asked**: the caller's badge, which the launcher records for ownership
  and checks its policy against.

## The kinds

A launched program is one of three kinds, and the kind is what the launcher
must hand the child:

1. **A command** -- shares the launcher's (a shell's) console stream; no
   window of its own. The launcher gives it the stream, the session namespace,
   the clock and timer, and `mem.main`; its memory grows through the service
   under its own badge (specs/memory.md). `Run` is this kind, without waiting.
2. **A windowed program** -- opens its own console window. The launcher hands
   it an unbadged `console.gui` so the child mints its own badge and its
   `attach` is its own first attach; the console gives a fresh process a fresh
   slice sized from its own windows, so no reservation and no growable slice
   are needed (specs/console.md). MultiView and an editor are this kind.
3. **A launching program** -- itself launches programs, so it also needs a
   spawn kit: an untyped, an ASID pool, and the unbadged ports it will hand
   its own children. A Terminal and a Workbench are this kind. A kind-3 child
   is a peer of its launcher, not a command.

A kind-2 or kind-3 child does not inherit the console stream: it has its own
window or its own stream. A kind-3 child's `mem.main` is the session's, so its
own commands still draw from the one pool.

## `NEWSHELL` and `NEWCLI`

They are the same act: start a new Shell, in a new window, carrying the
caller's context. `NEWCLI` and `NEWSHELL` are synonyms (the Amiga's own words),
and their arguments are the Amiga's:

- an optional window specification (`NEWSHELL "CON://640/200/My Shell/CLOSE"`,
  or `WINDOW=<spec>`), defaulting to the standard Shell window;
- `FROM <file>` -- the startup script instead of `S:Shell-Startup`.

    > NEWSHELL FROM S:Programming.startup

The new window becomes the selected one; it is a separate process, so its
input, output and program execution are independent while its cwd, prompt,
path, environment and stack are the ones it inherited.

## Who fulfills a launch, and where it lives

The capability a launch needs -- an untyped, an ASID pool, an unbadged
`console.gui`, the unbadged ports -- is expensive, and splitting a copy per
launcher would divide the session's memory and ASID pools among the Terminal,
the Bureau, the dock and every desktop icon. So there is **one launcher per
session**, a service that holds the kit and serves `launch.session`; every
launcher is a client of it. "Who asked" is the caller's badge on the call, and
the launcher's policy -- which badges may launch which kinds -- is the same
shape as the VFS's `may_resolve` (specs/vfs.md, specs/authority.md).

The session's launcher is started with the session, by auth, which is what
holds the session's unbadged `console.gui` and the kits today (specs/auth.md).
The Terminal stops being a boot-started special case: it is a kind-3 program a
session launches, and the boot session is simply its first launch. The
Terminal, in turn, is a launcher client: the Shell's `Run` and command lines go
to the same `launch.session` the icons and the Bureau will use, so a command
and a MultiView differ only in the request's kind.

## Phases

- **Phase 1 -- `Run`.** Landed (specs/memory.md Phase 5). The terminal can run
  a program without waiting, as one more process under its own `mem.main`
  badge and limits; it is kind 1's asynchronous form. The request shape here
  is the terminal's own `run`, which this spec later generalizes.
- **Phase 2 -- the launch request and the runtime API.** `aegir::launch` and
  the C runtime's spawn primitive build a request from the caller's own
  context; `launch.session` carries it. The terminal serves it for the first
  cut (it already holds the kit), and the Shell's lines and `Run` route
  through it, so the interface does not change when the launcher moves out.
- **Phase 3 -- kinds 2 and 3.** The launcher hands a windowed program its
  console and a launching program its kit; `NEWSHELL`/`NEWCLI` launch
  `aegir-terminal`, and a windowed program of the session (a viewer) is the
  second customer.
- **Phase 4 -- the launchers.** The Bureau's Execute, a dock and the desktop
  icons become launcher clients, each sending its own context.

## What this is not

- **A `fork`.** There is no address-space copy; the child is a new process the
  launcher builds. The *interface* is `fork`/`exec`-shaped; the mechanism is a
  spawn (specs/authority.md).
- **Unlimited.** A badge's kinds are policy. A command need not be allowed a
  window; a session's launcher bounds how many kind-3 peers exist, because the
  ASID pool and the pool are finite.
- **A process group or job control.** Stopping, signalling and reaping a
  session's tree is its own arc (specs/shell.md's later list).
- **Portable POSIX.** `aegir::launch` and the C spawn primitive are Aegir's;
  they fill the same role `fork`/`exec` do, not its whole API.
