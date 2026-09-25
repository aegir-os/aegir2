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
| `copy` | copy files or directories | `FROM/M`,`TO/A`,`ALL/S` |
| `delete` | delete files or directories | `FILE/M/A`,`ALL/S`,`FORCE/S` |
| `makedir` | create a directory | `NAME/M` |
| `rename` | rename a file or directory | `FROM/M/A`,`TO/A` |
| `list` | list a directory's entries in detail | `DIR/M`,`ALL/S` |
| `type` | display a text file | `FROM/M/A` |
| `more` | page a text file | `FILE/A` |
| `join` | concatenate files | `FROM/M/A`,`AS/K/A` (the Amiga's `TO` alias too) |
| `sort` | sort a file's lines | `FROM/A`,`TO/A` |
| `search` | find a string in files | `FROM/M`,`SEARCH/A`,`ALL/S` |
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
- **Phase 4 — the first commands.** Landed: `copy`, `delete`, `makedir`,
  `rename`, `list`, `type`, one hosted program each in `Sys:C`; `Dir`, `List`
  and `Type` left the shell's built-ins. Three pieces the tools needed came
  with them, each the abstraction a tool should have found and did not: the
  hosteded `aegir-command` stand-up (the runtime kit once, not six times),
  `sendfile` in the runtime (libc++'s `copy_file` is `sendfile` on Linux), and
  `delete`'s own tree walk (the runtime's `*at` calls do not anchor on a
  directory fd yet, which `std::filesystem::remove_all` needs -- implemented
  and reverted once because it broke the fs smoke). Their templates take the
  Amiga's argument lists -- `From/M`, `File/M/A`, `Name/M`, `From/A/M`,
  `Dir/M`, `From/M/A` -- so one line names several files. That needed
  `aegir::args` to reserve a later positional its argument, as ReadArgs does;
  otherwise `Copy From/M To/A`'s `To` was unreachable, and the parser is a
  pure value with host conformance cases now (`make check-args`).
- **Phase 5 — the rest of the set,** in slices: `more`/`search`/`sort`/`join`,
  `filenote`/`protect`, `info`/`assign`/`which`/`version`. Landed: `search`,
  `sort`, `join`, `more`, `protect`, `filenote`. `join`'s destination is a
  keyword (`AS`, the Amiga's `TO`) because that is the Amiga's template,
  `File/M/A AS=TO/K/A`, and `FROM` repeats. `more` needed the console stream's
  blocking read, which landed with it: the terminal grants each command a
  `con.doorbell` and rings it, the runtime's `read` parks on it, and a line
  typed while a command ran reaches the shell after it exits
  (`specs/terminal.md`). The page is the stream's `size`, not a constant.

  `filenote` is a BFS attribute, `AEGIR:COMMENT` (`specs/bfs.md`), reached
  through the POSIX xattr calls the runtime maps onto the metadata protocol;
  an absent `COMMENT` removes the note. A filesystem with no attributes
  refuses with `EOPNOTSUPP`, and the message names the volume's filesystem --
  `FAT32 does not support attributes` -- which the namespace's new
  `describe_path` supplies (`specs/vfs.md`). The boot test asserts FAT's
  refusal and the type's travel; FAT is the interchange filesystem, so its
  volumes are public (`specs/vfs.md`), and the acceptance reaches one: the
  last `filenote` is on the FAT16 volume and is the error path.

  `protect` is the ownership arc's mode made a command: its Amiga letters map
  r/w/e to the POSIX read/write/execute bits for every class, and the runtime
  grows `SYS_fchmodat`/`SYS_fchmod` onto the volume protocol's Protect
  (`specs/bfs.md` decision 7). Only the owner or the system class may, and a
  filesystem with no modes refuses.

  `rename`'s `From/A/M` is the Amiga's, but the volume protocol's rename is
  same-directory only (`specs/vfs.md`), so the multi-source move into another
  directory waits on a cross-directory rename there; the acceptance renames one
  file within its directory.

## The leak the toolset exposed

The richer command set first ran out of the terminal's memory after a handful
of commands. It was not capacity: 16 MiB failed where 4 did, with a large free
piece still reported, because the allocator listed pieces the kernel had
already spent. Three bugs, all fixed here:

- The terminal's repaint recomputed the display order (UAX #9) of every visible
  line on every paint, allocating per line and per paint; a repaint of unchanged
  text is common, and the churn grew the heap until a retype found none.
  `TerminalBuffer` now carries a version, and `TerminalView` caches the visible
  rows' cells, recomputing only when the text or the viewport changed
  (`specs/terminal.md`).
- The cache was still rebuilt on every keystroke, and a full UAX #9 pass
  allocates a dozen vectors per visible line, so the heap still grew a page a
  paint. `visual_cells_into` fills the view's own row buffer, whose capacity is
  reused, and a line whose characters cannot reorder (no R, AL, AN or explicit
  control) keeps the logical order without the algorithm. RTL text still takes
  the full path (`specs/terminal.md`).
- The allocator's `refill` left a partially-consumed parent on its free list
  after a failed split, so a later split retried a piece with no room and failed
  the allocation. A parent that cannot yield its children now leaves the list,
  and the loop looks for the next piece; the child a half-split did produce is
  kept. A piece the heap will never free also returns its node to the pool.

With all three, the acceptance runs the whole set in one session, several files
to a line.


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

Phase 1 and 4 together, landed: after the demo closes, the runner types a
`makedir` that creates two directories on the session's Home, a `copy` of two
files from `Sys:` into the first, a `list` of both, a `type` of both, a
`search` of two files for a word one holds, a `sort` into the first, a `join`
of a file with itself `AS` a second file there, a `more` of `Sys:LONG.TXT` (a
file longer than the window, so it pages and waits for a key), a `rename` of
one file within its directory, a `protect` of it to `rwe`, a `delete` of both
trees, and a `filenote` of a name on the FAT16 volume -- each a program read
from `Sys:C`, started by the terminal, resolving the session's namespace on its
own badge (the terminal's namespace copy). Most lines name several files, which
is the Amiga's argument-list shape; `more`'s key is typed while it runs, and
the rename line queued behind it runs after it exits. The FAT volume is public
because FAT is the interchange filesystem (`specs/vfs.md`), so the session
resolves it; it has no attributes, so the last `filenote` names the filesystem
and returns 10 -- the error path. The pixel checks prove the output reached the
grid and not a serial line.

Phase 3's, landed: the boot image's `Sys:C` holds the command set and the disk
it lives on is sized from them -- `make_disk.py` reports the AEGIR partition's
size as its tree's, not a constant, and the image boots. `Sys:` also carries
`LONG.TXT`, the pager's file.

The runner cues each step on the name of the command that just started, and
every cue is unique: it fires a step on *every* match of its trigger, so a
repeated cue would type its line more than once.
