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

**The device a program expects is the view's too.** `/dev/null` is Aegir's `NIL:`
— the device whose reads are EOF and whose writes are dropped (`specs/vfs.md`),
which is exactly what POSIX means by it — and it is asked for by name: LLVM opens
`/dev/null` to decide whether its output has colours
(`projects/llvm-project/llvm/lib/Support/Unix/Process.inc:231`), so a compiler
cannot print without it. Nothing here binds a `dev:` volume, so the view answers
it the way it answers every POSIX path: the volume's name is folded as the
namespace folds any first component, the name below it is kept as written, and
`NIL:` takes any name, so `/dev/null` is `NIL:null` (`libs/aegir-posix/include/
aegir/posix/path.h`). Nothing else under `/dev` is special, and both are asserted
by `make check-posix-path`.

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
acceptance below. **Files**, **memory** and **environment and time** have
followed: the write side of the file surface, `mmap` (a file's bytes included),
`mprotect`, `munmap` and `MAP_FIXED`'s refusal, and the environment, `uname` and
the clock a program reads -- each sub-arc with an acceptance client of its own
(below). What is left are the calls a POSIX program makes on top of those:
signals and threads, the two the endpoint's own table says it does not need
(`LLVM_ENABLE_THREADS=OFF`, and crash handlers disabled or stubbed,
`specs/clang-on-aegir.md:96-97`) -- and then the surface is whole. The on-device
compiler is the first large client, and it is what measures it
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
- **its evidence is a marker it prints itself.** `printf` is a libc call, not the
  runtime's diagnostic writer, so printing `AEGIR_POSIX_PATH_OK` is still
  something a program that does not know it is on Aegir can do, and the
  acceptance's step cues on that line. A check that fails exits non-zero, so the
  cue is never printed and the run fails naming it.

  A marker rather than an exit status, because of what runs the client: the
  session's `Sys:S/Shell-Startup` is a script, and a script stops at the first
  command that exits non-zero. Measured, on the run that put the file sub-arc's
  client one line behind this one with a success status of 62: this client exited
  63, the script stopped, and the file client -- staged under `Sys:Development/C`
  and on the disk -- never started at all. A non-zero success status can only
  ever belong to the last line of that script, which makes it a trap for every
  sub-arc that follows.

The rules above are pinned down without a boot as well: `aegir/posix/path.h` is a
pure function of the path and the current directory, and `make check-posix-path`
asserts its cases on the host (`scripts/check_posix_path.py`) -- where a
translation mistake lands as a line rather than as a path that resolves to the
wrong volume. The guest proves the view over the namespace and the volume
protocol.

### The files sub-arc's acceptance

`aegir-posix-file-test` is the client: a plain program -- `open` with `O_CREAT`,
`write`, `read`, `lseek`, `fstat`, `ftruncate`, `truncate`, `rename`, `unlink`,
`mkdir`, `rmdir`, `opendir`, `readdir`, and nothing else -- that carries no Aegir
call of its own. It works on the volume the tree already treats as its scratchpad
(`SCRATCH:`, reached as `/SCRATCH/...` through the view), because a write test has
to write somewhere and the AEGIR volume's own content stays as deployed.

What it proves is the writing half of the file surface over that volume's own
protocol: a file created through the view with a directory component in its path,
written in one call longer than a volume block, seeked, read back byte for byte,
sized, truncated through the descriptor and then by name, renamed (the old name
gone, the new one listed), and removed; a directory made, filled and removed; and
a name that was removed reading as absent rather than as a handle that outlived
it. It says `AEGIR_POSIX_FILE_OK` and exits 0; a failed check names itself and
exits 1, leaving its step's cue unprinted.

The file calls themselves needed no new mechanism: `libs/aegir-posix/src/files.cc`
already answered them over the volume protocol's `Write`, `Mkdir`, `Remove`,
`Rename` and `Truncate` methods, which the run measured rather than assumed. What
the sub-arc found were two things about the acceptance rather than the layer, both
measured on the guest:

- **the session's script stops at the first command that exits non-zero.**
  `Sys:S/Shell-Startup` is a script. A client whose *success* is a non-zero status
  can therefore only ever be its last line: with the path view's client exiting 63
  and the file client's line behind it, the file client never started -- staged
  under `Sys:Development/C` and on the disk the whole time. The rule is that a
  client's success is 0 and its evidence is a marker it prints, which is the shape
  the process sub-arc's client already had.
- **a client's `printf` reached its terminal, but not the acceptance.** The
  dispatcher already serves fd 1/2 from the session's console stream
  (`libs/hosted/aegir-heap/src/heap.cc`), so a plain program's output lands on the
  grid — while the acceptance's runner reads the serial, where only the runtime's
  own writer (`aegir::debug_write`, which is how the process sub-arc's client
  prints) had ever appeared. The two meet in the terminal now: every write a
  stream delivers is mirrored to the serial verbatim
  (`apps/hosted/aegir-terminal/src/console_stream_server.cc`), so what the runner
  matches is what a program printed, and a client that carries no Aegir call is
  cued on like any other.
- **a write did not move the descriptor's cursor.** `lseek(fd, 0, SEEK_CUR)`
  after a write answered 0, because `libs/aegir-posix/src/files.cc`'s write
  advanced the volume's own handle and left the descriptor's mirror alone. The
  client's seventh check is exactly that, and the acceptance named it — which is
  what the marker rule is for. `pwrite` remains the one that must not move it.

**The standard descriptors are answered, and `fstat` on them is what made a
compiler speak.** Fds 0, 1 and 2 are the console the runtime routes them to, and the
fd table does not hold them: the terminal owns their stream (`specs/shell.md`), so
`write` reached it while `fstat` answered `EBADF`. LLVM's
`Process::FixupStandardFileDescriptors` fstats stdout and stderr and, on `EBADF`,
`dup2`s `/dev/null` over both (`projects/llvm-project/llvm/lib/Support/Unix/
Process.inc:210-242`) — so a program linking LLVM lost everything it printed, which
is why clang ran and said nothing (`specs/clang-on-aegir.md`'s Phase 3). `fstat`
answers them now as they are: a character device, readable and writable.

### The memory sub-arc's acceptance

`aegir-posix-memory-test` is the client: a plain program — `mmap`, `munmap`,
`mprotect`, `open`, `close`, `memcmp`, and nothing else — that carries no Aegir
call, prints `AEGIR_POSIX_MEMORY_OK` and exits 0, and names a failed check with
`2+n`. It maps `/AEGIR/AEGIR.TXT` through the path view and compares the *file's
own bytes* through the mapping, which is the whole of what a compiler's
`MemoryBuffer` asks of a file; writes through an anonymous mapping and reads it
back; changes a page's protection with `mprotect` and checks that the mapping
still reads its bytes and takes a write again once it is widened, which is what a
program that fills a region and then narrows it relies on; releases the mapping
with `munmap` and checks that the mapping which follows lands zeroed, which is
what a recycling `munmap` owes a program that turns over large allocations
(`specs/memory.md`); and pins `MAP_FIXED`'s refusal, because a mapping that lands
quietly somewhere else is worse than one that fails.

`mprotect` needs the frame behind each mapped page in hand — a mapping's rights
change by issuing the Map invocation again at the same address
(`kernel/manual/parts/vspace.tex:294`), and that invocation names the frame — so
the heap keeps a record of them. Three findings shaped it, each measured on the
guest, and each a trap for whoever comes next:

- **the record must cost what is *used*.** One table sized from the whole arena at
  init cost the launcher the untyped it spawns commands with — `spawn: FAIL no
  untyped for the command's runtime` — because a launcher's window is large. The
  record is chunks of 512 frames, each chunk mapped the first time a page in its
  range is mapped.
- **`mmap` must not narrow.** Mapping a fresh page with the rights its `prot` asked
  for cost the cxx smoke a fault on a page it had mapped: its thread stack is
  `mmap`'d and written, so a mapping that arrives less permissive than the caller
  asked for is a regression the protection does not buy back. `mmap` maps
  everything writable, and `mprotect` is the only thing that narrows.
- **the rights are applied in place, with no unmap first.** An unmap whose remap
  does not succeed leaves the process without memory it owns; unmapping first was
  tried and cost the smoke a fault on an address it had mapped.

The dispatch trace (`-DAEGIR_HEAP_TRACE` on `aegir-heap`, read back with
`scripts/heap_trace.py`) is what located the second two: it named the faulting
address and the operations that preceded it.

### The environment-and-time sub-arc's acceptance

`aegir-posix-env-test` is the client: a plain program — `getenv`, `setenv`,
`unsetenv`, `environ`, `uname`, `sysconf`, `getcwd`, `chdir`, `clock_gettime`,
`nanosleep` and `printf`, and no Aegir call of its own — that prints
`AEGIR_POSIX_ENV_OK` and exits 0, and names a failed check with `2+n`.

Most of what it checks was already there, and the client is what says so: the
environment rides the startup frame's `argc`/`argv`/`envp`
(`bootstrap.h:158-159`), so `getenv`, `setenv`, `unsetenv` and `environ` work in a
hosted program with nothing of ours behind them; `clock_gettime` and `nanosleep`
were answered (the clock and timer services), and the monotonic clock moves across
a sleep; `sysconf`'s page size, clock tick and processor count are answered too.
The sub-arc's single gap was `uname`, whose riscv64 number (160) fell through to
`-ENOSYS`.

So the one mechanism this sub-arc added is the answer to `uname`, and it lives in
one place: `libs/freestanding/aegir-release`'s `aegir/release.h` holds what this
system says it is — sysname `Aegir`, nodename `aegir`, release `0.1`, version
`Aegir riscv64`, machine named per architecture, no domainname — and
`libs/aegir-posix`'s `system.cc` copies those words into musl's `struct utsname`
(six 65-byte fields, the kernel's own layout) when the dispatcher's case 160 calls
it. The machine is the one part the target decides, so a target the header does not
know is a compile error rather than a wrong answer.

What the client *inherited* it prints rather than asserts: the session's
environment is the running system's business, and a client that pinned a value
would fail for a reason that is not the layer's.

## What this is not

Not a change to Aegir's native paths, `std::filesystem`, or the namespace — the
Amiga grammar is the default and stays it. Not an FHS: there is no `/bin`,
`/etc`, `/usr`; a volume is a mount point, and an assign is what a program would
call a mount. Not per-process namespaces (a later extension,
`specs/namespace.md`).
