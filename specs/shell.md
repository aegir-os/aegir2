# shell: the Amiga command line

Status: decided (2026-09). This is the spec the shell arc lands under —
the command interpreter that runs in a `CON:` console (`specs/terminal.md`)
and, in its later phases, launches the commands it names.

The Amiga Shell is a small program: it prints a prompt, reads a line, and
either does something itself or runs a program from disk and waits. The
console it runs in is not its own (`specs/terminal.md`): the `CON:` handler
owns the window and the line editor, and the shell reads lines and writes
text through a stream. That separation is what lets a command print without
knowing anything about windows, and it is what makes the shell's job small
enough to state in one file.

## The decisions

- **The shell is a `CON:` client.** It opens a cooked console stream and
  loops: print the prompt, `read_line`, act. It holds no window, draws no
  text of its own, and knows no pixels. Its output is `write`.
- **The current directory is the process's, inherited and inherited from.**
  The shell reads its starting directory from `aegir::environment`
  (`specs/environment.md`), which for a session is `Home:` —
  auth binds `Home:` and gives the session `cwd = Home:` at spawn
  (`specs/auth.md`). A command the shell starts inherits that directory the
  same way, because inheritance is the environment's default. The shell
  never invents a starting directory; it is given one.
- **Changing directory is implicit, as the Amiga does it.** A line whose
  first token is not a command but names a directory changes the current
  directory — `Work:`, `Devs/`, `..`, `Home:` — with no `CD` written. The
  resolution order is the Amiga's: a command is looked up first, and only a
  name that is *not* a command is considered as a directory. So a directory
  and a command of the same name do not collide, and typing a volume name is
  a directory change. `CD` (or `CurrentDir`) with no argument prints the
  current directory; with an argument it is the explicit form of the same
  change. This is the behavior the user asked for by name; it is the
  Amiga's, and it is deliberately not POSIX's "command not found, then
  give up".
- **Built-in commands are the shell's, external ones are programs.** The
  line editor, `CD`/`CurrentDir`, `Echo`, `Set`/`Get` and the environment
  family (the `ENV:` toolset, `specs/environment.md`), the alias commands,
  `Prompt`, `Why`/`Fault`, `Eval` and `EndCLI`/`EndShell` are built in: they are
  about the shell's own state or its line, and an external binary for each
  would be ceremony. Everything else is a program — including `Dir`, `List`,
  `Type`, `Date` and `Wait`, which begin as built-ins and leave with the DOS
  toolset (`specs/dos.md`). `Quit` aborts a script, not the shell, and belongs
  to the interpreter (`specs/dos.md`'s Later arcs).
- **Commands come from `C:`, an alias of `Sys:C`.** A command name resolves
  through the namespace to its binary, which the terminal reads and hands to
  the spawner. `C:` is a **union alias** — `Sys:C` and `Home:C`, the `ENV:`
  pattern (`specs/namespace.md`) — searched in order, bound per badge by auth.
  No `PATH` variable: the Amiga assigns a directory to `C:`, and Aegir's
  aliases are the same mechanism, per badge. The first two commands resolved
  from the flat initrd (`Initrd:<name>`, Phases 3–6) while `C:` did not exist;
  the DOS toolset drops that lookup for the real set (`specs/dos.md`). A
  command's name is lowercased before it resolves, because a command is
  `C:copy` and the filesystem is case-sensitive.
- **Scripts live in `S:`.** The session's `S:` is a per-badge alias of the
  user's `Home:S`, made and bound by auth (`specs/auth.md`), and it is where
  the shell looks for its startup file (`specs/boot.md` for the boot
  sequence). The system's `Sys:S` is a separate name: a lookup tries `S:`
  first and falls back to `Sys:S`, with no union, so the system's scripts
  never appear in the user's listing and editing them needs elevation. Names
  are matched in the filesystem's own case — the shell does not fold them.
- **A command inherits the console as its standard input and output.** The
  shell hands the spawned program a copy of its console stream (capability
  transfer, the protocol's stated property). So `printf` reaches the grid,
  and while the command runs the terminal routes the keyboard to that
  stream's input queue -- the command reads it raw, so `read` sees keys
  (`specs/terminal.md`'s `read`; this is design A, the raw *view* of the
  shell's stream, rather than a stream per command). How a hosted program's
  fd 0/1/2 map onto that stream is the runtime's, and is Phase 4 below.
- **The shell waits, and reports the status.** A spawned command's exit is
  observed, and a non-zero status prints the Amiga-style line (`return code
  10`). Waiting is the shell's loop, not the console's.

## The shape

The shell is a program with a small loop; the pieces are the line, the
namespace, and the spawn.

### The loop

    open a cooked console stream with the prompt
    loop:
        line = read_line()
        if line is empty: continue
        words = split(line)
        if words[0] is a built-in: run it
        else if words[0] names a directory: set the current directory
        else if words[0] resolves to a command: spawn it with the console,
             wait for its exit, print its status if non-zero
        else: unknown command

The prompt is the current directory plus `>` — the Amiga's prompt is the
current directory, and Aegir shows the volume-and-path form with the colon
kept, so a login opens on `Home:>` and a directory under it is `Sys:Devs>`. It
is drawn by the handler's line editor as the prompt it was
opened with; after a directory change the shell updates it.

### Resolution: `C:CommandName`

A command name with no volume resolves through the `C:` alias — `Sys:C` then
`Home:C` (`specs/dos.md`) — and its lowercased name is what is looked up. A
name with a volume (`Sys:Utilities/Hello`) resolves directly through the
namespace and is run. This is the Amiga's `/`-path rule, applied to Aegir's
`Volume:rest` grammar (`specs/vfs.md`). A name that resolves to a directory
rather than a file is the implicit directory change; a name that resolves to
nothing is "unknown command". Phases 3–6 resolved the two first commands from
the flat initrd (`Initrd:<name>`); Phase 7 replaces that lookup with `C:`.

### The command's exit

`wait` and the status are a mechanism the shell does not own: today a
spawned process sends only its supervision notification, and `exit()` does
not even do that (`specs/authority.md`, `specs/environment.md`). The
mechanism — a status the supervisor can read, and an `exit()` that sends it —
is Phase 4's, recorded here so the shell's `return code` line has a source.

## The phases

The shell arrives in pieces, each provable on its own; the pieces below are
this arc's record of the order.

- **Phase 1 — the text surface.** `specs/terminal.md`'s `TerminalView`,
  input completeness, scrollback, BiDi and width. The ground the shell
  stands on; no shell yet.
- **Phase 2 — the CON: handler and the command line.** Landed: the terminal
  process owns the window and runs the shell in it, over the `LineEditor`
  (the cooked line editor and history, `specs/terminal.md`) -- **built-in
  commands only** — `CD`, `Dir`, `Type`, `Echo`, `Quit`. Typing a directory
  changes the current directory; `CD` prints it. The shell was the terminal's
  in-process client at this point; the `con.stream` port and the shell as a
  separate process landed with Phases 3 and 6. This
  is the first slice a person can use, and it needs no spawn at all.
- **Phase 3 — external commands.** Landed. Resolve a name to `Initrd:`, spawn
  it with the console stream, wait, report the status. The spawn authority a
  session is meant to have (`specs/authority.md`: a session spawns user
  processes "as ordinary use") is the terminal's, delegated by auth: a spawn
  untyped, a copy of auth's ASID pool, and the unbadged `spawn:` ports, which
  the terminal adopts into the toolkit's one allocator and window (a process
  has one VSpace root). The name is read out of `Initrd:` through the
  namespace as bytes and handed to the spawner as `binary_image` -- the whole
  initrd is 5.7 MiB and does not fit a child. The command inherits a caller
  copy of the shell's console stream, so its output lands on the same grid,
  and reports its status through the stream's `exit` method (the interim
  until `exit()` carries one, Phase 4); the shell prints the Amiga `return
  code N` for a non-zero status. The first command is `aegir-echo`. Two
  interims are recorded: the command's badge is a placeholder, not yet the
  session's user badge (which needs the process's own badge in the bootstrap
  block), and it holds only the console stream, no namespace or log.
- **Phase 4 — standard input and output.** Landed. The
  hosted runtime routes fd 1/2 to the console stream when the process has one
  (the debug serial otherwise, so every boot service is unchanged), so a
  command's `printf` reaches the grid; and `exit(status)` -- or a plain
  `return` from main, through the hosted runtime's exit callback -- reports the
  status through the stream, so the shell's return-code line comes from the
  command's own exit rather than from Aegir's API. `aegir-print` is the first
  command that uses libc and nothing else. fd 0 is the stream's queued input: while a
  command runs the terminal routes the keyboard to the stream rather than the
  idle editor (design A), and the command's `read` -- and `readv`, for
  buffered `fread` -- drains it. `aegir-read` is the command that proves it,
  reading fd 0 and echoing to fd 1. The read is a *poll*: a command that asks
  in a loop lets the terminal run between asks, and a read that waits for
  input is the later method (`specs/terminal.md`). Two
  interims remain from Phase 3: a command's badge is still a placeholder (the
  session's user badge needs the process's own badge in the bootstrap block),
  and a command holds the console stream and its runtime untyped, no namespace
  or log.
- **Phase 5 — the DOS toolset.** `Set`/`SetVar` and `Get`/`GetVar` over
  `aegir::environment` (`specs/environment.md`): the shell's own variables,
  which a spawned command inherits because the shell passes its environment on
  (`environ()`), so `Set exitcode 9` then `aegir-print` exits 9. Landed, with
  the persistent half: `auth` binds `ENV:` -- the union of the user's
  `Prefs/Env-Archive` (first, the create target) and the system's -- and the
  shell reads the merged view once at startup (`load_environment`) into
  `aegir::environment`. `Set` writes the variable to `ENV:<name>`, so a create
  lands in the user's archive; `Type ENV:exitcode` reads it back through the
  union. The system archive ships in the image, so a session's first command
  already knows `exitcode`.
- **Phase 6 — the shell as its own process.** Landed. `aegir-shell` opens a
  cooked stream on the terminal's `con.stream`, passes its own doorbell on
  `open`, and loops: `read_line`, run the built-ins, and for a command ask the
  terminal to `run` it. The terminal keeps the spawn authority (the pool, the
  ASID pool and the `spawn:` ports are still auth's delegation to it), and the
  shell's environment and current directory ride in the `run` call, so the
  command inherits what the shell set; the terminal reports the exit through
  the stream and the shell reads it with `command_status`. The terminal spawns
  the shell once, from a pool of its own (`auth`'s `shell-pool`), in
  `on_started` after the ready cue -- the spawn reads a 260 KiB image and would
  otherwise delay the cue past the demo's zoom. The line editor, the history
  and the grid stay the terminal's; only the words moved. Two interims from
  Phase 3 remain: the command badge is still a placeholder, and a command holds
  only the console stream and its runtime untyped.
- **Phase 7 — the DOS toolset.** `specs/dos.md`: the CLI commands as hosted
  programs in `Sys:C`, one binary each, with `ReadArgs` templates
  (`aegir::args`), and the plumbing that lets a command touch files without
  touching the kernel — the terminal mints it a `vfs.namespace`, a `clock.main`
  and a `timer.main`, badged with the session, beside the console stream.
  `Dir`, `List`, `Type`, `Date` and `Wait` leave the built-ins; the commands
  arrive in slices. The
  command-badge interim above is not this phase's: the namespace the command
  is given carries the session's identity even while the process's own badge
  is the placeholder, so `Home:`/`ENV:`/`C:` resolve and writes land owned by
  the session.

  The clock and timer halves landed with the programs that ask the time:
  director hands auth `spawn:clock.main` and `spawn:timer.main` (the manifest's
  `session.terminal` entry is what declares the need), auth hands the terminal
  the unbadged copies, and the terminal mints one of each for every command as
  `clock.main`/`timer.main` (specs/dos.md, specs/timer.md). `Date` reads the
  clock, formatted UTC -- there is no timezone in the image -- and `Wait`
  sleeps on the timer, so the clock says what time it is and the timer measures
  the interval (specs/timer.md).

## What this is not

- **Scripts.** `Execute` and a command language (redirection, pipelines,
  variables beyond `ENV:`, control flow) are a later arc. Tier 1 splits a
  line into words and acts; a quoting rule for names with spaces is the
  first thing that arrives with it.
- **Globbing and tab completion.** Completion belongs to the handler's line
  editor, and is deferred with the rest of the editor's polish.
- **A POSIX shell.** No `$`, no `&&`/`|`, no `exec`; Aegir is not POSIX
  (`specs/userland.md`), and the Amiga's single command line is the model.
- **Elevation.** A `Run`-as-root path through `auth` and director is
  `specs/authority.md`'s open list, not this spec.
- **The command set itself.** Which programs ship in `C:` is the DOS
  toolset's (`specs/dos.md`); this spec says how one is named and run.

## Acceptance

Phase 4's, landed: `aegir-print` is a hosted command, so the runner running it
proves the runtime: its `printf` reaches the grid through fd 1, and its `exit`
reports the status through the stream, which the terminal's `command exited`
cue carries and the shell's `return code N` line puts on the same grid. The
terminal window's pixels are read back too. `aegir-read` proves fd 0: while it
runs the runner types a line at the console, the keys queue on the stream, and
its `read` drains them and exits 0 -- the terminal's `command exited 0` cue is
the proof.

Phase 5's, landed for the in-memory half: the runner types `set exitcode 9`,
then `aegir-print` with no argument once the demo has closed (the runner fires
steps by cue, so the demo's zoom would take the focus mid-typing); the command
inherits `exitcode=9` from the shell and exits 9, which the terminal's
`command exited 9` cue reports. `Get` prints the value to the grid.

The persistent half is the same run's first and third commands. The system
archive ships with `exitcode` 11, so the shell's startup read loads it through
the union and the very first command -- `aegir-print`, before any `Set` --
exits 11; the 11 can only have come from `Sys:Prefs/Env-Archive`. The runner
then types `set exitcode 9`, `type ENV:exitcode` and `aegir-print`: the Set
writes the variable to the user's archive (the create target) and the Type
reads it back through the union, and the command inherits the override and
exits 9. That the shell resolves `ENV:` at all proves its namespace carries the
session's badge: `auth` grants the terminal a `shell:vfs.namespace` copy badged
with the session, and the terminal moves it to the shell.

Phase 6's is the same run: every line the runner types is read by `aegir-shell`
in its own process and every command is one the terminal started on its
request, so the whole command line -- `set`, `aegir-print`, `aegir-read`, and
the history recall -- is the shell-as-a-process path.

Phase 7's is `specs/dos.md`'s acceptance: the same run types a `makedir`, a
`copy`, a `list` and a `type`, and each is a program read from `Sys:C` and
resolving the session's namespace on the badge the terminal gave it -- while
`set`/`type`/`echo` stay the shell's own words.

`Date`/`Time`'s is the same run: the runner types `date` and `time` before the
first command, and the shell reads the clock the session was granted and
formats it (UTC). A session without a clock prints the no-clock line instead,
so the built-in reports the absence rather than inventing a time.
