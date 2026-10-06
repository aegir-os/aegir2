# launch: starting a program from a session

Status: decided (2026-09). The request shape, the kinds and the authority are
below. `Run` (kind 1) landed as specs/memory.md Phase 5; Phase 2 (`aegir::launch`
over the runtime primitive, the shell routed through it) landed. Phase 3 has
landed: the launcher kit and the reserved badge ranges, with `NEWSHELL`/`NEWCLI`
starting a peer terminal and its `WINDOW=`/`FROM` arguments -- served now by the
session's launcher itself, with the terminal left to own the stream
(specs/signal.md Phase 3). A program opens a
window whenever it wants one -- a command carries its own `console.gui` among
the grants, so nothing classifies it as "windowed" before it runs. `aegir-view`
(the MultiView shape) is the first program to use it, started by its bare name.

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
    aegir::launch::spawn(argv, argv_length, aegir::launch::kKindCommand, "", 0);

    /* C */
    aegir_spawn(argv, NULL, NULL, 0, NULL, kKindCommand);

`aegir::launch` adds no policy of its own: it builds the request and calls the
launcher (below). A program with no launcher capability gets a refusal, the
same way a `fork` in a system that caps processes does. A program that wants a
window opens one itself, with the `console.gui` every command is handed.

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
  size") is a property of the call, not of plumbing the caller writes. The
  `path` is the `Path` search list: a **bare** program name is resolved
  through it, in order, while an assign or volume path is used as typed and a
  path with `/` against the current directory (specs/dos.md).
- **the kind**, below;
- **the window specification**, when the caller gives one: the Amiga
  `CON:x/y/width/height/title/options`, which the program that owns the window
  parses. A program launched by bare name uses its own default; the field is
  what a `NEWSHELL WINDOW=` (and, later, a desktop icon) rides to name the
  window it wants;
- **the caller's console stream**, when it has one: the request's one
  capability, and the only one it carries (`specs/signal.md` -- a message
  carries one capability, so nothing can ride beside it). A command the launcher
  starts writes where its caller does, and its readiness is the stream's own (a
  held `read`), so no notification travels with the request. A caller with no
  stream sends none, and the launcher gives the command a stream of its own: a
  read-only output view, one per launcher, started the first time a stream-less
  request arrives (`aegir-output`, below);
- **who asked**: the caller's badge, which the launcher records for ownership
  and checks its policy against.

## The kinds

A launched program is a **command** or a **launching program** -- the wire's
`kKindCommand` (1) and `kKindLaunching` (3). Kind 2, a separate "windowed
program" request, is retired: it classified a program before it ran, which is
exactly what the launcher should not do.

- **A command** shares the launcher's (a shell's) console stream, and is handed
  its own `console.gui` among the rest -- the session namespace, the clock and
  timer, `mem.main` (its memory growing through the service under its own badge,
  specs/memory.md), and a caller half of `bureau.menu`, so it registers the menus
  the screen bar shows while it is active: any Workbench program's menus, not
  only a boot service's (specs/workbench.md). It opens a window whenever it
  wants the GUI. `Run` is this kind, without waiting.
- **A launching program** itself launches programs, so it also needs a spawn
  kit: an untyped, an ASID pool, and the unbadged ports it will hand its own
  children. A Terminal and a Workbench are this kind. A launching child is a
  peer of its launcher, not a command.
- **A class** (kind 4, `kKindServe`) is a program that *serves* a port instead
  of writing a stream (`specs/datatypes.md`). The request's one capability is a
  port the caller made; it is installed under `datatypes.class`, the caller keeps
  the other half and calls it, and the program runs as the caller's user class,
  exactly as a command does. This is the primitive a *user* resource library
  needs: the class runs as whoever asked, and only the asker can reach its port
  (`specs/libraries.md`).

A launching child does not inherit the console stream: it has its own window
and its own stream. A launching child's `mem.main` is the session's, so its own
commands still draw from the one pool.

## The read-only output view

A launching program -- a Terminal, a Workbench, the Bureau -- has its own window
and no console stream of its own, so a command it starts has nowhere to write.
The launcher gives it somewhere: on the first stream-less request it starts
`aegir-output` once, and keeps the endpoint the view owns. The view is granted
`aegir-spawn-kit`'s `output_ports`: a command's grant with the `con.stream` cap as
an *owner* copy -- the view serves it, the command calls it -- plus the launcher's
`launch.session` caller half. It draws the bytes it is written into a text grid,
answers a read empty (it takes no input; it is a view, not a terminal), and, when
the command's exit report arrives -- carrying the command's own badge -- releases
the command through `launch.session` (`kMethodRelease`), because no shell holds
the command's line to reap it. One view serves a session's stream-less commands;
the next one's output appears in the same window.

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
The boot session gets one too, for the same reason a login's does: its
`Sys:S/Startup-Sequence` is a sequence of commands, and a command is started by
the service that holds the kit, so auth starts a `system.launcher` beside the
boot terminal and hands the terminal its caller half (specs/boot.md). The
Terminal stops being a boot-started special case: it is a launching program
a session launches, and the boot session is simply its first launch. The
Terminal, in turn, is a launcher client: the Shell's `Run` and command lines go
to the same `launch.session` the icons and the Bureau will use, so a command
and a MultiView are the same request -- the MultiView simply attaches its
window.

## Phases

- **Phase 1 -- `Run`.** Landed (specs/memory.md Phase 5). The terminal can run
  a program without waiting, as one more process under its own `mem.main`
  badge and limits; it is kind 1's asynchronous form. The request shape here
  is the terminal's own `run`, which this spec later generalizes.
- **Phase 2 -- the launch request and the runtime API.** Landed. `aegir::launch`
  (C++) and the C runtime primitive `aegir_spawn` (over `aegir_launch_request`)
  build a request from the caller's own context; the terminal served it on the
  same endpoint as con.stream for the first cut, and `launch.session` -- the
  launcher's own endpoint -- serves it now (specs/signal.md Phase 3), under the
  method numbers `kMethodSpawn`/`kMethodPipeline`. The shell's command lines,
  `Run` and pipelines all route through it. `argv`, `cwd`, `environment`,
  `path`, the redirections, the window specification and the stack ask all
  travel, and the request carries the caller's con.stream as its one capability;
  the launcher fulfills kind 1.
- **Phase 3 -- the launcher kit and the windowed program.** Landed. A
  launching child is built from the launcher kit: auth delegates the first-cut
  launcher, the terminal, an unbadged `spawn:console.gui`, so the child mints
  its own and its console `attach` is its own first. The launcher draws the
  child's runtime untyped and shell pool from `mem.main` on demand under its
  own badge (specs/memory.md), gives the child a reserved badge range so no two
  of a session's processes share a serial, hands it the session's namespace by
  copy, and gives it a larger CSpace (specs/authority.md) so it can launch in
  turn. The kit itself is one module (`libs/freestanding/aegir-spawn-kit`): the
  launcher builds its commands and a nested terminal, the terminal only its own
  shell, all with the same builders auth uses, so no spawner reassembles the
  list -- and since specs/signal.md Phase 3 it is the launcher that builds the
  commands and the nested terminals. `NEWSHELL`/`NEWCLI`
  launch `aegir-terminal` this way, and a nested terminal stands up as a peer
  with its own window and shell; their `WINDOW=<spec>` (or a bare `CON:...`) is
  the child's window, carried in the request's own field for the child to
  parse, and `FROM <file>` rides as the program's arguments, so the child's
  shell runs it in place of Shell-Startup. A command, meanwhile, now carries a
  badged `console.gui` in its grant, so it opens its own window whenever it
  wants one -- the launcher no longer tells a "windowed program" apart from a
  command before it runs. `aegir-view` (the MultiView shape, its file read
  through the granted namespace) is the first program to use it, launched by
  its bare name like any other command (specs/console.md).
- **Phase 4 -- the launchers.** Landed in part: the Bureau's Execute is a
  launcher client. A Bureau menu item (`Execute...`) opens a `Requester` with a
  `TextBox`, splits the line to argv, and sends it through `launch.session` with
  the Bureau's own context. The Bureau has no console stream, so this is also
  where the launcher's **read-only output view** lands: a caller that sends no
  stream is given one by the launcher, which starts `aegir-output` once -- a
  program that owns the `con.stream` endpoint the command writes to, draws what
  arrives in a window, and releases the command through `launch.session`
  (`kMethodRelease`) when it sees the exit, because no shell holds the command's
  line to reap it. The requester takes focus through a blocking call, so the
  Bureau prints `bureau: execute ready` once `show()` has returned and the focus
  is the requester; the acceptance types only after reading that cue. Keys sent
  before then land on the window focused before it -- or nowhere, since the
  console drops a key with no focus -- and the command loses its head (the
  `info` -> `nfo` flake). A dock and the desktop icons are still to come.

## What this is not

- **A `fork`.** There is no address-space copy; the child is a new process the
  launcher builds. The *interface* is `fork`/`exec`-shaped; the mechanism is a
  spawn (specs/authority.md).
- **Unlimited.** A badge's policy decides what it may launch. A session's
  launcher bounds how many launching peers exist, because the ASID pool and the
  pool are finite; a command's own memory still grows under its badge
  (specs/memory.md).
- **A process group or job control.** Stopping, signalling and reaping a
  session's tree is its own arc (specs/shell.md's later list). Naming one
  process and interrupting it -- the Amiga's Break, the registry, and the
  enforced halt that reuses this spec's teardown -- is `specs/process.md`.
- **Portable POSIX.** `aegir::launch` and the C spawn primitive are Aegir's;
  they fill the same role `fork`/`exec` do, not its whole API.
