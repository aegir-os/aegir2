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
  line editor, `CD`/`CurrentDir`, `Dir`/`List`, `Type`, `Echo`, `Set`/`Get`
  (the `ENV:` toolset, `specs/environment.md`), and `Quit`/`EndCLI` are built
  in: they are about the shell's own state or the namespace, and an external
  binary for each would be ceremony. Everything else is a program.
- **Commands come from the flat initrd first, and `C:` later.** Aegir has no
  command path yet. The initrd is already a volume of flat names
  (`specs/services.md`), so the first slice resolves a command name to
  `Initrd:<name>`, reads those bytes, and hands them to the spawner. When
  command volumes exist, `C:` arrives as a **union alias** — `Sys:C` and
  `Home:C`, the `ENV:` pattern (`specs/namespace.md`) — searched in order,
  and the initrd lookup is dropped. No `PATH` variable: the Amiga assigns a
  directory to `C:`, and Aegir's aliases are the same mechanism, per badge.
- **A command inherits the console as its standard input and output.** The
  shell hands the spawned program a copy of its console stream (capability
  transfer, the protocol's stated property). So `printf` reaches the grid
  and a raw reader sees keys. How a hosted program's fd 0/1/2 map onto that
  stream is the runtime's, and is Phase 4 below.
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
current directory, and Aegir shows the volume-and-path form `Home>`,
`Sys:Devs>`. It is drawn by the handler's line editor as the prompt it was
opened with; after a directory change the shell updates it.

### Resolution: `Libs:CommandName` and `Initrd:CommandName`

A command name with no volume resolves in command order; a name with a
volume (`Sys:Utilities/Hello`) resolves directly through the namespace and
is run. This is the Amiga's `/`-path rule, applied to Aegir's `Volume:rest`
grammar (`specs/vfs.md`). A name that resolves to a directory rather than a
file is the implicit directory change; a name that resolves to nothing is
"unknown command".

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
  changes the current directory; `CD` prints it. The shell is the terminal's
  in-process client for now; the `con.stream` port and the shell as a
  separate process need the spawn authority, so they land with Phase 3. This
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
- **Phase 4 — standard input and output.** Landed for output and exit: the
  hosted runtime routes fd 1/2 to the console stream when the process has one
  (the debug serial otherwise, so every boot service is unchanged), so a
  command's `printf` reaches the grid; and `exit(status)` reports the status
  through the stream, so the shell's return-code line comes from the command's
  own exit rather than from Aegir's API. `aegir-print` is the first command
  that uses libc and nothing else. fd 0 is routed to the stream's poll but the
  handler queues no input yet, and a command that wants the keyboard wants a
  raw stream of its own -- the next piece, with per-client streams. Two
  interims remain from Phase 3: a command's badge is still a placeholder (the
  session's user badge needs the process's own badge in the bootstrap block),
  and a command holds the console stream and its runtime untyped, no namespace
  or log.
- **Phase 5 — the DOS toolset.** `Set`/`SetVar` and `Get`/`GetVar` over
  `aegir::environment` (`specs/environment.md`): the shell's own variables,
  which a spawned command inherits because the shell passes its environment on
  (`environ()`), so `Set exitcode 9` then `aegir-print` exits 9. Landed. The
  persistent half -- the `ENV:` union and the `Sys:`/`Home:` `Prefs/Env-Archive`
  files, so a variable survives a login -- is the next piece, with the union
  binding `specs/namespace.md` defines.

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
- **The command set itself.** Which programs ship in `C:`/`Initrd:` is the
  storage and toolset arcs'; this spec says how one is named and run.

## Acceptance

Phase 4's, landed: `aegir-print` is a hosted command, so the runner running it
proves the runtime: its `printf` reaches the grid through fd 1, and its `exit`
reports the status through the stream, which the terminal's `command exited`
cue carries and the shell's `return code N` line puts on the same grid. The
terminal window's pixels are read back too. fd 0's queued input, and a
command's own raw stream, are the next piece.

Phase 5's, landed for the in-memory half: the runner types `set exitcode 9`,
then `aegir-print` with no argument once the demo has closed (the runner fires
steps by cue, so the demo's zoom would take the focus mid-typing); the command
inherits `exitcode=9` from the shell and exits 9, which the terminal's
`command exited 9` cue reports. `Get` prints the value to the grid; the
`ENV:` union and the archive files are the next piece.
