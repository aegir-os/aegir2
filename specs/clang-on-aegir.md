# Clang on Aegir

Status: plan, for review (2026-09). **Nothing below is implemented.** This file
records the decisions a first implementation can start from, so that the plan is
not carried in a conversation. It extends `specs/userland.md` (Aegir is not
POSIX), `specs/cxx.md` (the hosted runtime and its completion program) and
`specs/build.md`, whose host build is now clang (*The compiler: clang*) — the
host half of this effort, and this document's prerequisite.

The goal is an **on-device native compiler**: `clang` and `lld` running as
ordinary Aegir processes under the seL4 kernel, reading source through Aegir's
VFS and emitting `riscv64` Aegir ELFs. This is not the host cross-compiler
`specs/build.md` defers; it is a program Aegir itself runs.

## The hard part: Aegir is not POSIX

`specs/userland.md:5` states the position: Aegir exposes its own API and does
not implement POSIX. LLVM and Clang are among the most host-dependent large C++
programs there are — their system layer calls the kernel directly:

| LLVM site | call |
| --- | --- |
| `llvm/lib/Support/MemoryBuffer.cpp` | `mmap` (file-backed) |
| `llvm/lib/Support/FileOutputBuffer.cpp` | `mmap` |
| `llvm/lib/Support/Unix/Memory.inc` | `mmap`, `mprotect` |
| `llvm/lib/Support/Unix/Program.inc` | `posix_spawn`, `fork`, `execve`, `sigaction` |
| `llvm/lib/Support/Unix/Signals.inc`, `CrashRecoveryContext.cpp` | `sigaction` |
| `llvm/lib/Support/Unix/Threading.inc` | `pthread_create` |
| `llvm/lib/Support/Unix/Path.inc` | `opendir`, `readdir`, `statvfs`, `getenv` |
| `llvm/include/llvm/Support/MemoryBuffer.h` | file mappings |

So "port clang" reduces to closing that gap. Two facts make it tractable rather
than heroic:

- **The syscall redirection is already in the tree.** The tracked musl patch
  (`third_party/patches/projects/musl/0001-riscv64-redirect-syscalls-to-sel4-vsyscall.patch`)
  routes *every* musl syscall through the `__sysinfo` function pointer, and
  `libs/hosted/aegir-heap/src/heap.cc` already owns that switch (`vsyscall`).
  Adding a POSIX surface is adding handlers, not re-plumbing.
- **LLVM's defaults already match Aegir's tier-1 runtime.** LLVM 20 defaults
  `LLVM_ENABLE_EH=OFF` and `LLVM_ENABLE_RTTI=OFF`, exactly what
  `scripts/build_libcxx.sh` builds libc++ with. Exceptions and RTTI are not a
  prerequisite here, unlike so much else.

The same fact cuts the other way: the dispatcher has grown well past the first
handful this plan named. `libs/hosted/aegir-heap/src/heap.cc`'s `vsyscall` — the
function musl's `__sysinfo` points at — now answers the whole file surface a
`std::filesystem` needs (`openat`, `close`, `read`, `lseek`, `newfstatat`,
`fstat`, `getdents64`, `mkdirat`, `unlinkat`, `renameat2`, `truncate`,
`ftruncate`, `chdir`, the xattr family, `sendfile`) over `aegir::vfs`, plus
`clock_gettime`/`nanosleep`, the socket family and `exit_group`. The gap map
below is corrected to the tree. What remains for a compiler is one load-bearing
call — file-backed `mmap` (the dispatcher's `sys_mmap` is anonymous-private
only, `heap.cc:513`) — and a small tail (signals, threads, process).

## The decisions

| Decision | Choice |
| --- | --- |
| Goal | On-device native compiler: clang+lld run under Aegir, emit `riscv64` Aegir ELFs |
| Non-POSIX gap | A POSIX personality behind `__sysinfo`; LLVM unpatched except tracked patches |
| Target triple | A first-class `riscv64-unknown-aegir-elf` (a tracked clang patch) |
| Components | clang + lld + LLVM core, `RISCV` target only; compiler-rt builtins / libc++abi; tooling **last** |
| Driver model | Stock `clang` and `lld`, each a child process, with `cc` a name for the driver; the driver starts `cc1` and the linker |
| Process surface | `posix_spawn` + `wait4` over Aegir's spawn (`specs/launch.md`); `fork` explicitly absent |
| First compile | A real program against the `Sys:Development` sysroot (musl + libc++ headers, libs and crt) |
| Acceptance driver | A dedicated manifest service that compiles at boot and prints the marker |
| Machine | `aegir-8g-smp4` (8 GiB, 4 cores) |
| First milestone | `specs/compiler.md`'s successor: this plan plus clang/lld cross-built (not yet running), sizes measured |

The architectural decision is `specs/userland.md:11-28`'s POSIX layer, which
that spec says "may be added later as an ordinary user-level service" (ixemul's
shape), and which `specs/cxx.md:190-193` explicitly distinguishes from
`std::filesystem`. The port is that layer's first large client.

### Rejected

- **Patch LLVM's `Unix/` system layer against Aegir's own API.** A large,
  upstream-hostile patch surface that fights the vendoring model
  (`specs/third_party.md`), and it would have to be redone at every LLVM bump.
  The personality route leaves LLVM untouched except patches that stand on their
  own.
- **A Unix-emulation server providing `fork`/signals wholesale.** More mechanism
  than the compiler needs, and it puts the POSIX surface in a process rather than
  where the syscalls already land.

## The gap map

Verified in the tree. Each row is a prerequisite arc.

| Capability | What LLVM/Clang needs | Aegir today |
| --- | --- | --- |
| File I/O | `open/read/write/lseek/fstat/stat/unlink/mkdir/opendir/readdir` | **done** — the dispatcher answers all of these over `aegir::vfs` (`libs/hosted/aegir-heap/src/heap.cc`) |
| Memory | file-backed `mmap`, `mprotect`, `MAP_FIXED`, real `munmap` | **Landed**: file-backed `mmap` (`sys_mmap`, `heap.cc`), `mprotect` (`sys_mprotect`, over a record of the frame behind each mapped page), and `munmap`/`mremap` recycling (specs/memory.md) — all measured by `aegir-posix-memory-test` (specs/posix.md). `MAP_FIXED` is refused by decision: the arena's free area is one interval, so a hole punched into it could not be handed back. |
| Process | `posix_spawn` + `wait4` for cc1 + ld; `fork` not needed | none; the POSIX process sub-arc is load-bearing (`specs/posix.md`) |
| Threads | `pthread_create`, mutex, atomics | `LLVM_ENABLE_THREADS=OFF` avoids them; a `clone` route exists (musl patch 0002) but is not on this path |
| Signals | `sigaction` (crash handlers) | none; disable or stub |
| Time/env | `clock_gettime`, `nanosleep`, `getenv`, `uname`, `getcwd` | **Landed**: `getenv`/`setenv`/`environ` work in a hosted program (the environment rides the startup frame's envp, `bootstrap.h:158-159`), `uname` answers from `aegir/release.h`, `sysconf` needs nothing of ours, and `clock_gettime`/`nanosleep`/`getcwd` were already answered — measured by `aegir-posix-env-test` (specs/posix.md). |
| C++ | libc++ **no EH/RTTI** | already built tier-1 (`scripts/build_libcxx.sh`) |
| Assembler/linker | integrated assembler + lld | cross-buildable from the vendored tree (Phase 1); not on device |

## The shape of the work

```
specs/clang-on-aegir.md                  # this file
scripts/build_llvm.sh                    # Phase 1: cross-build clang+lld+builtins
libs/aegir-llvm/imported.cmake           # Phase 1: expose clang/lld to CMake
third_party/patches/projects/llvm-project/0002-riscv64-aegir-triple.patch  # the one new LLVM patch
libs/aegir-posix/                        # Phase 2: the personality (files, mem, env, time)
libs/hosted/aegir-heap/src/heap.cc       # shrinks: the file handlers move to libs/aegir-posix
apps/aegir-cc/                           # Phase 3: the sysroot's clang, lld and cc (stock programs)
apps/aegir-clang-test/                   # the acceptance service
```

### Phase 0 — this document, and the corrections it forces

This plan is the deliverable. Writing it also settles three housekeeping points:

- `specs/cxx.md` carried a "Clang 22" note of the host cross-compile; the host
  compiler is now pinned at **clang 20.1.x** (`specs/build.md`), and the
  on-device compiler is the vendored **LLVM 20.1.8** (`manifests/aegir.xml:87`).
  That note is corrected.
- `THIRD-PARTY.md` does not list `projects/llvm-project/` at all, though the
  manifest pins it and the hosted runtime builds libc++ from it. A row is owed
  (Apache-2.0 with LLVM-exception), and clang/lld's role belongs in it.
- `specs/build.md`'s compiler choice is now decided — the host build is clang
  20.1.x (*The compiler: clang*) — and this document is the next step from it,
  not a merge into it: that document builds Aegir's ELFs with clang on the host;
  this one runs clang on Aegir.

### Phase 1 — cross-build `clang` + `lld` (landed)

`scripts/build_llvm.sh`, modelled on `scripts/build_libcxx.sh`, cross-compiles
LLVM, clang and lld as **libraries** against the already-built `musl_full` and
libc++, exposed by `libs/aegir-llvm/imported.cmake`. The programs come from those
libraries the way every hosted program comes from its own: **Aegir's CMake links
them**, from clang's and lld's own tool sources, in `apps/hosted/aegir-clang` and
`apps/hosted/aegir-lld`. So `build_llvm.sh` builds no executables and
`CLANG_BUILD_TOOLS`/`LLD_BUILD_TOOLS` stay **off**. The earlier plan had LLVM's own
CMake link them with Aegir's recipe supplied as link flags, and the reason it
changed is that Aegir's CMake already knows that recipe (the `sel4runtime` crt,
seL4's link groups, musl, libc++ and the eh-frame script, `CMakeLists.txt:151-182`),
while the runtime archives such a link names do not exist until Aegir's build has
run once — a second pass, for a link we would have had to re-express inside a
foreign CMake. The configuration, and why:

| Option | Value | Why |
| --- | --- | --- |
| `LLVM_ENABLE_PROJECTS` | `clang;lld` | the frontend and the linker, nothing else |
| `LLVM_TARGETS_TO_BUILD` | `RISCV` | one backend; the size win is real |
| `LLVM_USE_HOST_TOOLS` | `ON` | cross-compiling needs native `llvm-tblgen`/`clang-tblgen`; LLVM builds them for us (`projects/llvm-project/llvm/CMakeLists.txt:817,1148`) |
| `LLVM_ENABLE_EH` / `RTTI` | `OFF` / `OFF` | Aegir's tier 1, and LLVM's own default |
| `LLVM_ENABLE_THREADS` | `OFF` | avoids the refused `clone`; single-threaded clang compiles one translation unit fine |
| `LLVM_BUILD_LLVM_DYLIB` | `OFF` | everything static; there is no dynamic loader |
| `LLVM_ENABLE_ZLIB/ZSTD/TERMINFO/LIBXML2/CURL/LIBEDIT` | `OFF` | no such dependencies on Aegir |
| `CLANG_ENABLE_STATIC_ANALYZER`, `LLVM_INCLUDE_TESTS` | `OFF` | not shipped |
| `LLVM_APPEND_VC_REV` | `OFF` | hermeticity: the same `GIT_CEILING_DIRECTORIES` concern as `dtc` (`specs/build.md`, *Build environment*) |
| `CLANG_BUILD_TOOLS` / `LLD_BUILD_TOOLS` | `OFF` | the programs are ours (above); LLVM's own `llvm-*` tools stay out, and the one stray target-side `llvm-tblgen` is what `CMAKE_EXE_LINKER_FLAGS` carries a placeholder for |

`compiler-rt`'s builtins for `riscv64` are built with the same cross toolchain;
they are freestanding and need no libc. This is the same recipe the host
migration uses for Aegir's own links (`specs/build.md`, *The compiler: clang*
step 3), so the two share it. libc++abi is already built by
`scripts/build_libcxx.sh`.

Two things the *programs* needed that the libraries did not, each recorded where
it lives: `__cxa_thread_atexit_impl`, which libc++abi's thread-local path calls and
musl's install here has no counterpart for (`libs/hosted/aegir-cxxabi-shim/src/
thread_atexit.cc`: accepted and dropped, because Aegir's programs are
single-threaded by decision); and, for clang's own tool sources, upstream's warning
policy rather than ours (`apps/hosted/aegir-clang/CMakeLists.txt`, the user's
decision, matching what the tree already does for LLVM's and clang's headers under
`specs/build.md`).

**Acceptance.** `clang` and `lld` exist as programs and `llvm-readelf` reports
static `riscv64` ELFs — 59496904 and 101699576 bytes, `EXEC`, no `PT_INTERP` —
sizes measured against the dedicated development disk (`specs/development.md`), and
any warning from our own patch is fixed (`AGENTS.md`; there is no patch). Deploying
them, and what a 97 MiB hosted program does after its stand-up, is Phase 3 — which
has its first measurement already.

### Phase 2 — the POSIX personality

A new `libs/aegir-posix`, with the handlers reached through the existing
`__sysinfo` switch, backed by Aegir's VFS, ports and seL4. The `/`-rooted path
view it presents — every volume and assign under `/`, native paths unchanged —
is `specs/posix.md`. The surface already exists, inside
`libs/hosted/aegir-heap`; the first step is therefore to move those handlers
into `libs/aegir-posix` and leave the heap the memory calls it owns, so the
surface grows in one place rather than in the heap. Each sub-arc has its own
acceptance client, independent of clang, because the rule is that the calls live
in a library and not a program (`specs/userland.md:149-156`):

1. **Files** — `open/close/read/write/lseek/fstat/stat/unlink/mkdir/opendir/readdir`
   over `vfs.namespace` and the volume protocol. The largest prerequisite, and it
   depends on the filesystem arc (`specs/cxx.md:218`); the volume protocol
   (`specs/vfs.md:137-198`) needs `seek` and `stat` methods it does not yet have.
2. **Memory** — file-backed `mmap`, `mprotect`, `MAP_FIXED`, and a real `munmap`.
3. **Environment and time** — `environ`/`getenv` on top of `aegir::environment`,
   `clock_gettime`, `nanosleep`, `uname`, `getcwd`/`chdir`, `sysconf`, `getpid`.
4. **Signals** — stubs, with LLVM's crash overrides disabled at build time.
5. **Threads** — real seL4 TCBs, per the rules already written down in
   `specs/userland.md:85-116`.
6. **Process** — `posix_spawn` and `wait4` over Aegir's spawn
   (`specs/launch.md`). Load-bearing: the stock `clang` driver starts `cc1` and
   the linker as child processes (`llvm/lib/Support/Unix/Program.inc:197`,
   `HAVE_POSIX_SPAWN`). `fork` stays absent; nothing here needs an
   address-space copy.

### Phase 3 — the compiler as programs

The compiler is the stock `clang` and `lld` (with `cc` a name for the driver),
each an ordinary Aegir program on the development tree. `clang` starts `cc1` and
the linker as child processes through the POSIX process surface (Phase 2.6), so
the argument handling, the include search and the link line are clang's own, not
ours. This reverses the earlier in-process-driver decision, which existed only
because the process surface was absent; `specs/launch.md` already names the spawn
as the mechanism and `posix_spawn` as the interface.

**Where the programs stand, measured.** They were deployed ahead of this phase to
see what they would do, and the answer corrected the question: there was no hang.
Markers either side of the driver call (`printf`, so the harnesses stayed free of
Aegir calls) showed both programs run to completion — constructor, `main`, driver,
return (`/tmp/aegir-run44.log`). Two real faults came out of it instead:

- **`lld` works on the guest.** It refused `lld --version` because lld takes its
  flavour from the name it is invoked under — its own diagnostic says which names
  it wants — and deployed as `ld.lld` it prints `LLD 20.1.8 (compatible with GNU
  linkers)` in the session's own startup, which is the dev target's cue now
  (`/tmp/aegir-run46.log`).
- **`clang` runs and prints.** Two faults, both the layer's, both fixed and both
  measured. It asked for `/dev/null` — LLVM opens it to decide whether its output
  has colours (`projects/llvm-project/llvm/lib/Support/Unix/Process.inc:231`) — and
  the view translated that to a `dev:` volume nothing binds; the view answers it now
  (`/dev/null` is Aegir's `NIL:`, `libs/aegir-posix/include/aegir/posix/path.h`,
  pinned by `make check-posix-path`). That fix is what exposed the second, and the
  real one: `Process::FixupStandardFileDescriptors` (the same file, :210-242) fstats
  stdout and stderr and, on `EBADF`, `dup2`s `/dev/null` over both — and `fstat`
  answered `EBADF` for the standard descriptors, because the fd table does not hold
  them (the terminal owns their stream, `specs/shell.md`). So every program linking
  LLVM was mute while its own libc calls printed, which is exactly the split the
  harness's markers showed. `fstat` answers them now — a character device, readable
  and writable — and clang prints `clang version 20.1.8` in the session's startup
  (`/tmp/aegir-run47.log`).

**And a cap came back, now that there are real programs.** Loading a 97 MiB and a
57 MiB program spends the spawn path's untyped, and nothing reclaims a departed
command's: placed *before* `aegir-big`, both made its spawn fail — `spawn: FAIL no
untyped for the command's runtime`, four attempts — which is the cap `aegir-big`
exists to break (`specs/development.md`). The session's startup therefore runs the
clients and `aegir-big` first and the big programs last, which is a workaround with
a reason rather than a fix: reclaiming a departed command's memory is the work that
names.

**And the acceptance's own progress, measured.** `development_tree` stages what it
needs — the sysroot's `Libs` as *one* merged archive (a volume holds only so many
names at all, `scripts/mkfs_bfs.py:231-236`; and a 4096-byte tree node works where
2048 refused the dev tree, `scripts/mkfs_bfs.py:32`), the source, and a generated
link line — and the session ran it: **the device compiled the source with clang**
(`cc --target=riscv64-unknown-elf -c` — clang's own driver and cc1, nothing of the
host in the loop), which is what the shell reaching the *next* line proves. The link
then stopped twice, and both are named: a two-kilobyte command line arrives
truncated, because a command's arguments travel in one envelope (fixed by reaching
the archive through `-L` and `-l:` in a few hundred bytes), and `ld.lld: cannot
open …/sysroot.a: Out of memory` — the unreclaimed memory again, with a 97 MiB
compiler and a 57 MiB linker already loaded. So the acceptance waits on that
reclamation, not on anything about the compiler.

**And the growths it needs are in, measured.** Two of those were AGENTS.md's "no
arbitrary or hardcoded limits" in substance: the *builder* refused any tree that
outgrew one 2048-byte B+tree node, and the heap refused a mapping when its
fixed-seed arena filled. Both grow now. `NODE = 4096` builds the development tree
with the sysroot staged beside it, and the volume mounts and serves it
(`scripts/mkfs_bfs.py:32`; a tree that outgrows even that wants the builder to split
leaves into a real multi-leaf B+tree, which the format allows and the service already
walks — `aegir/bfs/bplustree.h:15-16`, `writer.h:180`, `src/volume.cc:641-672`). And
the heap's arena takes the window it is given rather than the seed alone
(`libs/hosted/aegir-heap/src/heap.cc`: `bytes` is a floor, not a ceiling), because
what bounded it before — a frame record sized from the arena at init — is chunked
now. Both are committed, the run is green with them, and the sysroot's staging
rides with them: one merged archive (members renamed by their archive, since six
names repeat and member names mean nothing to a linker), the crt objects, the
stand-up, the eh-frame script, clang's own headers beside their program, the source,
and a generated *short* link line.

What the acceptance still stops on is one thing, and it is not the compiler. The
compile runs on the device — clang reads `Sys:Development/hello.c` and writes an
object — and `ld.lld` then reports `cannot find linker script
Sys:Development/aegir-eh-frame.lds` and `unable to find library -l:sysroot.a`. Those
files are staged, verified on the host, and *flat in the directory clang just read*:
the staging was moved out of a `Libs` subdirectory on the first measurement of this,
and the second measurement failed identically. So the fault is lld's own file access
on a path it is handed — `-T`, `-L`, `-l:`, and whatever it does before `open` — and
that is where this picks up. The session's startup does not invoke the acceptance
yet, because a failing link stops the session's script and starves every command
after it.

A third measurement rules out the obvious explanation: the staging was moved out of
the `Libs` subdirectory into `Sys:Development` itself, so the script and the archive
sit exactly where `hello.c` sits — and `ld.lld` failed identically, in the same run
whose compile through that directory succeeded. (That arrangement also cost 27 cues
elsewhere in the run, so it is reverted; the sysroot stays in `Libs`.)

A fourth places the fault in lld's *search* rather than in the layer. Pointing `-T` at
a staged file that is certainly there — `crt0.o`, beside the script — produced the
*same* "cannot find linker script", while lld's attempt at a file that genuinely was
not there reported the *layer's own errno*: `ld.lld: error: cannot open
SCRATCH:hello.o: No such file or directory`. So lld reaches this filesystem and its
`open` reports errors faithfully; what fails is how it decides a script or a `-l:`
library *exists* before opening it — `fs::exists` over the search paths, which is
`stat` where `open` already works. That is the one call to look at next, and it is
why the compile succeeds while the link cannot find its inputs.

The read of that call puts the layer on the hook after all, one level down.
`newfstatat` reaches the file through `stat_target` (`libs/aegir-posix/src/files.cc`),
which shares `resolve_target` with `openat` — the same resolve clang's read of
`Sys:Development/hello.c` already proves — but then asks the volume differently:
`aegir::vfs::Volume::stat(rest, length, info)` through a `transient_slot()`, where
`openat` goes through the open path. So the probe is `stat` against that one path,
and the suspects in order are `Volume::stat`'s reply for a member of a *nested*
directory, the transient slot, and `fill_kstat`'s kind. The check is cheap and needs
no compiler: a client that `stat`s `Sys:Development/Libs/crt0.o` (staged, present)
the way the path view's client `stat`s `/AEGIR`.

Evidence already in hand narrows that to one suspect, and it is the walk. `stat` on a
*depth-1* nested file works — LLVM's `MemoryBuffer::getFile` stats before it opens, and
clang's compile of `Sys:Development/hello.c` succeeded, so `fs::status` was answered
there. Depth-2 *resolution* works too: the shell spawns `Sys:Development/C/cc`. What
has never worked is `stat` on a *depth-2 nested file* — `Sys:Development/Libs/crt0.o`,
and the linker script beside it. So `openat`'s walk and `Volume::stat`'s walk disagree
past one component, and the fix belongs in the volume client's stat
(`libs/freestanding/aegir-vfs-client`) rather than in `libs/aegir-posix/src/files.cc`.
The one-run probe above still names it outright.

`apps/aegir-clang-test` is the acceptance service: it runs at boot, compiles a
known program against the `Sys:Development` sysroot, links it, spawns the
result, and prints the marker.

### Phase 4 — tooling (libclang, clangd, clang-tidy)

Last, and effectively a separate effort. clangd and clang-tidy need real
threads, `std::filesystem`, signals and process control — roughly doubling
Phase 2 — and their memory appetite may sit past the envelope. They are recorded
as in scope for the *effort* but not on the critical path, and the first
milestones must not wait on them.

## The target triple

The compiler must identify Aegir as its own target rather than borrow
`riscv64-unknown-elf`. The decision is a first-class `riscv64-unknown-aegir-elf`,
carried as a tracked patch
(`third_party/patches/projects/llvm-project/0002-riscv64-aegir-triple.patch`).
The patch has to touch:

- the `Triple` above (arch/vendor/os/environment), so `isOSBinFormatELF`, the
  default ABI and the object format are right;
- a Driver toolchain for the triple, so `-march=rv64imafdc_zicsr_zifencei
  -mabi=lp64d` and (later) the sysroot and the in-process linker are selected by
  default;
- the compiler-rt builtins directory name, which embeds the triple.

Until the sysroot exists the toolchain is deliberately minimal: no default
libraries, no crt, freestanding by construction. The patch is ours and must be
re-checked at every LLVM bump (`specs/third_party.md`'s upgrade list).

## Cross-cutting

- **Disk.** The compiler is on the order of 100–200 MiB. The acceptance disk is
  small (`scripts/make_disk.py`) and cannot hold it; `specs/development.md` gives
  the compiler its own target and larger disk, so the disk the existing tests
  stand on — created once, and answered through the per-run overlay in
  `scripts/run_target.py` — is not grown.
- **Envelope.** The 2 GiB floor cannot host a compiler; development and
  acceptance are on `aegir-8g-smp4`. That is a capacity finding for
  `specs/aegir.md` to carry, not a quiet change of the floor.
- **Vendoring.** LLVM is already pinned (`manifests/aegir.xml:86`); no new fetch
  is introduced. The triple is a tracked patch, applied idempotently and verified
  by `make deps-check`.
- **Architecture.** The personality and the driver stay architecture-neutral; the
  triple and `-march`/`-mabi` are the only `riscv64` facts, and they live in the
  target configuration (`AGENTS.md`'s abstraction rule).
- **Process rules.** Long LLVM builds run under `timeout` and are checked for
  orphaned QEMU afterwards; each phase is its own commit; capacity tables grow on
  demand.

## Acceptance, stated once

On `aegir-8g-smp4`, the `aegir-clang-test` service compiles, links and runs a
known source file entirely inside the guest, and prints its marker. The host
cross-build (Phase 1) is accepted separately by the binaries and their measured
sizes; it does not require them to run.

## Open, for review

- **The exact triple spelling.** `riscv64-unknown-aegir-elf` is the intended
  normalization; whether the vendor field should be `unknown` or something
  Aegir-specific is a detail to fix with the patch.
- **The hosted tier's build triple.** LLVM's Support library selects its Unix
  implementation by macro and has no generic path (`Unix/Process.inc` `#error`s
  without one). Aegir's hosted C runtime is musl and its syscall ABI is Linux's
  — the `__sysinfo` dispatcher answers Linux riscv64 numbers — so the hosted
  tier is built for `riscv64-unknown-linux-musl`, not the freestanding tier's
  `riscv64-unknown-elf`. Whether to convert the whole hosted tier (musl, libc++,
  the hosted apps) to that triple, so libc++ inline header code cannot differ
  between translation units, is open; today only `scripts/build_llvm.sh` uses
  it. The compiler's *output* triple (above) is a separate thing again.
- **Where the compiler lives and how it is reached.** It is a development tool
  on the system volume, run directly from the session as
  `Sys:Development/C/clang` (`specs/development.md`); it is not a `Sys:C`
  command and not a boot service. The boot acceptance starts it from a manifest
  service.
- **On-device sysroot packaging.** Designed in `specs/development.md`: the
  compiler ships in `Sys:Development` (`C`, `Include`, `Libs`), a program on the
  system volume run from the session, versioned with the runtime.
- **`posix_spawn`, decided.** It is needed and on the critical path: the stock
  `clang` and `lld` are separate processes. `fork` stays absent — an
  address-space copy is not something seL4 provides (`specs/launch.md`) — and
  nothing here needs one. The surface is `posix_spawn` + `wait4` over Aegir's
  spawn (Phase 2.6).
- **Threads off, or on.** Building with `LLVM_ENABLE_THREADS=OFF` defers real
  threads to Phase 2.5. Whether Phase 3 should instead enable them depends on
  measured compile behavior under QEMU.
- **The LLVM revision, host and device.** Both are **20.1.8**: the host build is
  clang 20.1.x (`specs/build.md`), and the vendored `projects/llvm-project` that
  builds libc++ and this on-device compiler was bumped to `llvmorg-20.1.8`, so
  they share a revision. A future bump is a `specs/third_party.md` upgrade
  exercise, not a subset of this plan.