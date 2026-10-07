# The POSIX layer

Status: decided (2026-10). This spec fixes what a POSIX program sees on Aegir —
a `/`-rooted view of the volumes and assigns — and where that view lives.
`specs/userland.md` states the position (Aegir is not a POSIX system; a POSIX
compatibility layer may be added later as an ordinary user-level service),
`specs/cxx.md` distinguishes the hosted C++ runtime's `std::filesystem` from it,
and `specs/clang-on-aegir.md`'s Phase 2 is its first large client. This is the
"POSIX layer" those name.

## The position

Aegir's filesystem is the Amiga's, and that is the **default everywhere**: a path
is `Volume:rest` (`specs/vfs.md`), the namespace maps volume names and assigns
(`specs/namespace.md`), and `std::filesystem` speaks that grammar (the tracked
patch `third_party/patches/projects/llvm-project/0002-aegir-path-grammar.patch`).
Nothing native changes.

The POSIX layer is a personality **bolted on at the libc boundary** — ixemul's
shape on the Amiga — and it is the only place a POSIX-style path exists. It is a
*library* (`libs/aegir-posix`), reached through the `__sysinfo` switch every musl
syscall already lands in (the switch itself is aegir-heap's `vsyscall`, which
forwards its cases across the library edge — the file surface and `posix_spawn`'s
pid table have moved there, `specs/clang-on-aegir.md`), not a service and not a
second grammar in the tree.

**If a program needs POSIX, it uses `aegir-posix` and sticks to C.** The C
surface (`open`, `stat`, `opendir`, …) is the personality. `std::filesystem` is
Aegir's, so it keeps the Amiga grammar and does not grow POSIX path semantics; a
ported C++ program that reaches for it is using Aegir's API, and a port that
needs a POSIX `std::filesystem` is a separate piece.

## The path view

Every volume and every binding (an assign, a union) hangs off `/` as one entry:

| POSIX | Aegir |
| --- | --- |
| `/` | the synthetic root: every volume and binding |
| `/Name` | `Name:` — the volume's or binding's root |
| `/Name/rest` | `Name:rest` |
| `foo`, `./foo` | a native path, relative to the current directory |

So `Sys:` → `/Sys`, `Initrd:` → `/Initrd`, `Net:` → `/Net`, `C:` → `/C`, and a
binding like `ENV:` → `/ENV` (a union reads and lists merged, which the VFS
already serves, `specs/namespace.md`).

`/` is not a volume. The layer synthesizes it from the namespace: the volume
table (`count`/`describe`) plus the bindings (`bindcount`/`binddescribe`,
`specs/namespace.md`). `stat("/")` is a directory, `opendir("/")` lists the
names, `stat("/Name")` is the volume's or binding's root. A volume that says it
has no directory (`kFlagNoDir`, `NIL:`) is listed under `/` but is not itself a
directory to browse.

## The boundary

The translation is one step at the `files` boundary: a path that begins with `/`
is POSIX and is rewritten to the native `Volume:rest`; anything else is native and
passes through unchanged. So `std::filesystem`, which emits `Sys:foo`, is
untouched, and a native program's paths never enter the translation.

**`.` and `..` are normalized here**, because the Amiga's directories have none
(`specs/vfs.md`): the layer drops `.` and resolves `..` against the composed path.
`..` above a volume's root is `/`; `..` at `/` is `/`.

**The first component is matched case-insensitively**, because volume and assign
names are the Amiga's (`aegir:` and `AEGIR:` are the same volume, `specs/vfs.md`);
below it a filesystem's own rule applies (FAT folds case, BFS does not). A
listing presents the name the volume registered.

**The current directory is one string, and it is a VFS path** (`specs/environment.md`):
`chdir` of `/Sys/Tests` stores the VFS's `Sys:Tests`, and a relative path is
composed with it before the translation. The layer keeps one string because the
native side reads the same `getcwd`: `std::filesystem::absolute` and LLVM's
`sys::fs::make_absolute` compose with it and expect a `Volume:` root, which is
what the on-device compiler needs to resolve its own arguments. So `getcwd`
answers in the grammar the directory was **set in** — a POSIX `chdir` keeps the
view's (`chdir("/Sys/Tests")`, `getcwd` → `/Sys/Tests`), a native one
(`chdir("Sys:DOCS")`) and the directory a process inherits from its spawner stay
Aegir's (`Sys:DOCS`), and `/` is its own answer. Answering the view's form
unconditionally is what the first cut did, and it cost both native readers
exactly that: the fs smoke's Aegir-grammar check went red and the compiler
stopped resolving the source it had just written.

**`/` is a directory to read, not a place to write.** `stat`, `open` with
`O_DIRECTORY`, `getdents` and `chdir` answer it from the namespace; a create or a
remove at `/` is refused the way any path that names no volume is, because a
volume is a mount point and `/` is the list of them (below, "What this is not").

## The arc

The path view is the first sub-arc, and it has landed: the translation and its
host conformance (`libs/aegir-posix`'s `aegir/posix/path.h`, `make
check-posix-path`), the synthetic `/`, and the current directory, with the
acceptance below. The rest are the calls a POSIX program makes on top of it, each
with its own acceptance client independent of any program: files
(`open`/`read`/`write`/`lseek`/`stat`/…), memory (file-backed `mmap`,
`mprotect`, a real `munmap`), environment and time, signals, threads, and process
(`posix_spawn` and `wait4`; `fork` is absent on purpose, `specs/launch.md`). The
on-device compiler is the first large client, and it is what measures the surface
(`specs/clang-on-aegir.md`).

### The process sub-arc's acceptance

`aegir-posix-test` is the client: a plain C program that calls `posix_spawn` and
`wait4` and carries no Aegir call, so the runtime's POSIX face is measured by a
program that does not know it is on Aegir. Its child is `aegir-posix-child`, a
program of its own that stands up the hosted runtime and returns from `main` --
nothing else.

The child is a **distinct program with a distinct name** on purpose, not an
existing command like `date`. The launcher announces every command it starts as
`command started <argv[0]>` (`specs/launch.md`), and the acceptance fires a step
on that cue; a child named `date` would fire the acceptance's own `date` step on
the *first* date -- the child -- and leave the acceptance's date with no press
(`scripts/run_target.py`, `AGENTS.md`'s trigger-uniqueness rule). Both live
under `Sys:Development/C`, which only the development target carries.

### The path view's acceptance

`aegir-posix-path-test` is the client: a plain program — `open`, `read`, `close`,
`chdir`, `getcwd`, `stat`, `opendir`, `readdir`, and nothing else — that carries
no Aegir call of its own, so what it measures is the view a program that does not
know it is on Aegir sees. It opens `/AEGIR/AEGIR.TXT` through the view, walks `.`
and `..`, chdirs and asks the directory back in both grammars, and browses `/`,
reading what it lists.

Two pieces make "carries no Aegir call" true rather than aspirational:

- **the runtime stands the process up.** `libs/hosted/aegir-crt0` is an *object*
  library with one constructor that calls the stand-up before `main`. Object, not
  archive, because a linker searches a static archive only for symbols something
  already references -- so a constructor inside one never runs in the program
  this exists for. The stand-up is idempotent, so a program that still opens with
  the call itself is unaffected; and a process that cannot be stood up exits 127
  rather than halting, because a halt is a process that never ends and the shell
  waiting on it would wait for ever.
- **its evidence is its exit status.** A program that must not know it is on
  Aegir cannot call the runtime's diagnostic writer to print a marker, so the
  acceptance's step cues on the terminal's own line for its exit
  (`terminal: command exited <n>`, `scripts/targets.py`) with a status no other
  command in the run carries -- the shape `aegir-echo`'s code already has in the
  DOS acceptance. A check that fails exits with a status no step cues on, so the
  cue is never printed and the run fails naming it.

The rules above are pinned down without a boot as well: `aegir/posix/path.h` is a
pure function of the path and the current directory, and `make check-posix-path`
asserts its cases on the host (`scripts/check_posix_path.py`) -- where a
translation mistake lands as a line rather than as a path that resolves to the
wrong volume. The guest proves the view over the namespace and the volume
protocol.

## What this is not

Not a change to Aegir's native paths, `std::filesystem`, or the namespace — the
Amiga grammar is the default and stays it. Not an FHS: there is no `/bin`,
`/etc`, `/usr`; a volume is a mount point, and an assign is what a program would
call a mount. Not per-process namespaces (a later extension,
`specs/namespace.md`).
