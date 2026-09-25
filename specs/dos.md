# dos: the command set

Status: decided (2026-09). This is the spec the DOS-toolset arc lands under —
the CLI commands of AmigaDOS, as Aegir's own: where they live, what they are
called, how they read their arguments, and how they are kept from touching the
kernel. `specs/shell.md` is the command line that runs them; this file is the
set.

The Amiga's command set is a flat directory, `SYS:C`, of small programs. `Dir`,
`Copy`, `Delete`, `Type`, `Set` and the rest are all one binary each; the Shell
finds them by name and runs them. Aegir does the same, with one difference the
kernel forces: a command here is a **hosted** program (`specs/cxx.md`,
`specs/userland.md`), so it reaches the filesystem and the clock through the
runtime's libraries and never through seL4. That the runtime can do so is the
whole reason this arc is short: `aegir-heap/src/files.cc` already issues every
`open`, `read` and `std::filesystem` call through `vfs.namespace` found by name,
and asks no more of a command than that the port be there.

## The decisions

- **A command is a program in `Sys:C`, one hosted binary each.** Not the
  initrd: the initrd is the boot set (`specs/services.md`), and a command is
  ordinary userland that changes without a boot image. The flat initrd lookup
  `specs/shell.md`'s Phase 3 used for the first commands is dropped for the
  real set, and the command volumes arrive as the `C:` alias the spec already
  anticipated.
- **`C:` is an alias, the `ENV:` shape.** It is bound per badge as a
  `specs/namespace.md` union: `Sys:C` the base, the user's `Home:C` appended
  when it exists, so a session can add commands without touching the system
  volume. No `PATH` variable: the Amiga assigns a directory to `C:`, and the
  per-badge alias is that assignment. auth binds it for a session and its
  terminal, exactly as it binds `Home:` and `ENV:`.
- **Names are lowercase and resolution is case-blind.** The Amiga is
  case-insensitive; Aegir's filesystems are not (`specs/vfs.md`). The
  convention that reconciles them: every command's binary is named in lowercase
  (`copy`, `delete`, `makedir`), and the shell lowercases the first word of a
  line before it resolves anything. So `Copy`, `COPY` and `copy` are one
  command, and `C:Copy` is spelled `C:copy`.
- **Built-ins are the shell's state; commands are programs.** The line editor,
  the implicit directory change, `CD`/`CurrentDir`, `Echo`, `Set`/`Get` and
  the environment family, `Alias`/`UnAlias`, `Prompt`, `Quit`/`EndCLI` are the
  shell's: they are about the shell's own state or its line. Everything that
  acts on the filesystem or the machine is a program in `C:`. `Dir`, `List` and
  `Type` move out of the built-in set with this arc.
- **Arguments are `ReadArgs` templates.** A command declares a template —
  `FROM/A,TO,ALL/S,DIR/S` — and `aegir::args` parses the line into named
  values. The amiga conventions are the ones adopted: `/A` an argument that
  must be present, `/S` a switch (present or absent), `/K` a keyword with a
  value, `/M` one that may repeat, `/N` a decimal number. The separator is a
  space or a comma, keywords are case-insensitive, and a missing required
  argument is refused with the template's own usage text. This is the shape
  `specs/environment.md` left open when it said a command-line parser was not
  its business.
- **A command calls no seL4.** It links the hosted runtime and the libraries
  (`std::filesystem`, `aegir::filesystem`, `aegir::environment`, `aegir::args`)
  and is given, as ports, `con.stream` (its standard input, output and error),
  `vfs.namespace` (its files) and `clock.main` (its time). The terminal grants
  them at spawn; the runtime finds them by name. Where a tool would otherwise
  have to reach for a slot, the answer is a missing library, not a syscall in
  the tool — the rule this arc exists to keep.
- **The command's ports carry the session's identity.** The `vfs.namespace`
  copy the terminal mints for a command is badged like the shell's, so `Home:`,
  `ENV:` and `C:` — the aliases auth bound for the session badge — resolve for
  a command as they do for the shell, and what a command writes is owned by the
  session rather than by a placeholder. (The process's own badge is the
  separate, still-open piece `specs/shell.md` records.)
- **Return codes are AmigaDOS's.** `0` is success; a command that could not do
  what it was asked returns non-zero and the shell prints its `return code N`
  line. The set uses the Amiga's bands: `5` a warning, `10` an error (`COPY`'s
  object not found), `20` a failure. A tool reports its reason to stderr and
  its band as its status; the shell's `Why` reads the last one back.
- **A command's streams are the console stream, raw.** fd 0/1/2 are the one
  stream the shell hands on (`specs/shell.md`'s design A): a command's `printf`
  reaches the grid and its `read` drains the input queue. `More` pages with a
  `read`, `Ask` reads a line; neither needs a feature the phase did not land.

## The command set

The classification is a filter, not an inventory — the GUI, the printers, the
disk utilities and the firmware tools of AmigaDOS are other arcs or none.

**External, in `C:` (hosted programs).** The file and framework set:

| Command | Does | Needs |
| --- | --- | --- |
| `copy` | copy files or directories | `FROM/A`,`TO/A`,`ALL/S`,`CLONE/S` |
| `delete` | delete files or directories | `FILE/A`,`ALL/S`,`FORCE/S` |
| `makedir` | create a directory | `NAME/A` |
| `rename` | rename a file or directory | `FROM/A`,`TO/A` |
| `list` | list a directory's entries in detail | `DIR/A`,`ALL/S` |
| `type` | display a text file | `FILE/A`,`NUMBER/S` |
| `more` | page a text file | `FILE/A` |
| `join` | concatenate files | `FROM`,`TO` |
| `sort` | sort a file's lines | `FROM/A`,`TO` |
| `search` | find a string in files | `FILE/A`,`SEARCH/A`,`ALL/S` |
| `filenote` | attach a comment to a file | `FILE/A`,`COMMENT` |
| `protect` | change a file's protection bits | `FILE/A`,`FLAGS/A` |
| `info` | describe the mounted volumes | `DEVICE` |
| `assign` | bind a namespace alias | `NAME`,`TARGET`,`ADD/S`,`REMOVE/S` |
| `which` | resolve a command name | `NAME/A` |
| `version` | report a file's version | `FILE` |

`MakeLink` waits on link support in the VFS; `Avail` and `SetDate` wait on
free-space and mtime-write in the filesystem interface.

**Built-in (the shell's).** The line's own commands: `CD`/`CurrentDir`,
`Echo`, `Set`/`Get`, `SetEnv`/`GetEnv`/`UnSet`/`UnSetEnv`, `Alias`/`UnAlias`,
`Prompt`, `Why`/`Fault`, `Eval`, `Date`/`Time`/`Wait`, `Quit`/`EndCLI`.

**Later arcs.** Scripting (`Ask`, `Execute`, `If`/`Else`/`EndIf`,
`Skip`/`Label`/`EndSkip`, `FailAt`, `Run`, `IconX`) needs the interpreter.
`Status` needs a process registry. The GUI, printer, font, serial, disk and
firmware commands are their own arcs or out of scope.

## The shape

### Storage and resolution

    C:copy      the alias resolved by the namespace, the bytes read by the
                terminal, the image handed to the spawner
    copy        what the shell sends the terminal, lowercased

The terminal owns the spawn authority (`specs/shell.md`'s Phase 6) and reads
the command's image from `C:<name>` through the namespace, the way it read
`Initrd:<name>` before. The image is near a megabyte and is read one command at
a time; `Sys:C` is a BFS directory (`specs/bfs.md`), so the binaries are files
on the system volume, built and packed into the image by `scripts/make_disk.py`.

### `aegir::args`

One header, no seL4 in a caller's translation unit:

    namespace aegir::args {
        // The parsed line: a value per template item, present or not.
        struct Result {
            bool had(char const *name) const noexcept;
            char const *value(char const *name) const noexcept; // nullptr when absent
            bool present(char const *name) const noexcept;      // an /S switch
            char const *usage() const noexcept;                 // the template's help
            char const *problem() const noexcept;               // why it failed, or ""
        };
        Result read(char const *template_text, int argc, char const *const *argv) noexcept;
    }

A command's whole `main` argument handling is one call; a refusal prints
`problem()` and returns 10.

### The command's environment

A command inherits the shell's `argc`/`argv`, variables and current directory
(`specs/environment.md`), so a relative path resolves against where the shell
stood and `GetEnv`-set names are visible. The shell passes its environment on
in the `run` call; the terminal passes it to the spawner unchanged.

## Phases

- **Phase 0 — this spec.** The classification, the decisions, and the hooks
  into `specs/shell.md`, `specs/environment.md`, `specs/authority.md`,
  `specs/services.md` and `specs/bfs.md`.
- **Phase 1 — the command is granted what it needs.** auth binds `C:` for the
  session and terminal badges; the terminal copies its own session-badged
  `vfs.namespace` into each command as it starts it -- a copy preserves the
  badge, so the command carries the session's identity beside `con.stream`. No
  command uses it yet; the path is proven with the first tool. The clock is
  granted the same way, by copy, with the first tool that asks the time.
- **Phase 2 — `aegir::args`.** The template parser and its acceptance.
- **Phase 3 — `Sys:C` on the system volume, sized from the set.** The
  commands are built, stripped, packed into `Sys:C`, and the AEGIR partition
  and the image are sized from what they weigh rather than from a constant.
  With the commands on `C:`, the terminal resolves an image from `C:<name>`
  instead of `Initrd:<name>` and the shell lowercases the command token.
- **Phase 4 — the first commands.** `copy`, `delete`, `makedir`, `rename`,
  `list`, `type`; and `Dir`/`List`/`Type` leave the shell's built-ins.
- **Phase 5 — the rest of the set,** in slices: `more`/`search`/`sort`/`join`,
  `filenote`/`protect`, `info`/`assign`/`which`/`version`.

## What this is not

- **Scripts.** `Execute`, redirection, pipelines and control flow are the
  interpreter arc's (`specs/shell.md`'s What this is not).
- **A POSIX toolset.** The commands are AmigaDOS's, with the Amiga's arguments
  and return codes, not `cp`/`ls`/`rm` with GNU options.
- **The GUI, the printers, the disks.** The Workbench commands, the printer
  drivers and the disk utilities are not this arc; where one is a CLI shell
  over a system call that does not exist yet, it waits on that call.
- **A `PATH`.** `C:` is an alias, assigned per badge; a command is not searched
  for across a list of directories.

## Acceptance

Phase 1 and 4 together: after the demo closes, the runner types a `makedir`
that creates a directory on the writable volume, a `copy` into it, a `list` of
it, and a `type` of the copy — each a program read from `Sys:C`, started by the
terminal, resolving the session's namespace on its own badge. A `type` of a
name that is not there returns 10 and the grid shows the `return code 10` line,
which is the error path. The pixel checks prove the output reached the grid and
not a serial line.

Phase 3's: the boot image's `Sys:C` holds the commands and the disk it lives on
is sized from them — `make_disk.py` reports the AEGIR partition's size as the
sum of its tree, not a constant, and the image still boots.
