# Clang on Aegir

Status: plan, for review (2026-09). **Nothing below is implemented.** This file
records the decisions a first implementation can start from, so that the plan is
not carried in a conversation. It extends `specs/userland.md` (Aegir is not
POSIX), `specs/cxx.md` (the hosted runtime and its completion program) and
`specs/build.md` (the deferred compiler choice).

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
  `libs/aegir-heap/src/heap.cc` already owns that switch (`vsyscall`, `:310`).
  Adding a POSIX surface is adding handlers, not re-plumbing.
- **LLVM's defaults already match Aegir's tier-1 runtime.** LLVM 18 defaults
  `LLVM_ENABLE_EH=OFF` and `LLVM_ENABLE_RTTI=OFF`, exactly what
  `scripts/build_libcxx.sh:91-92` builds libc++ with. Exceptions and RTTI are not
  a prerequisite here, unlike so much else.

The same fact cuts the other way: the current dispatcher answers eight calls —
`brk`, `mmap`, `munmap`, `mremap`, `madvise`, `write`, `writev`, `exit`
(`libs/aegir-heap/src/heap.cc:215-352`) — and a compiler is file-heavy and
process-aware. The gap map below is the work.

## The decisions

| Decision | Choice |
| --- | --- |
| Goal | On-device native compiler: clang+lld run under Aegir, emit `riscv64` Aegir ELFs |
| Non-POSIX gap | A POSIX personality behind `__sysinfo`; LLVM unpatched except tracked patches |
| Target triple | A first-class `riscv64-unknown-aegir-elf` (a tracked clang patch) |
| Components | clang + lld + LLVM core, `RISCV` target only; compiler-rt builtins / libc++abi; tooling **last** |
| Driver model | Single process: `-fintegrated-cc1` / integrated-as + lld linked in-process |
| First compile | Freestanding (`-ffreestanding -nostdinc`, a tiny crt); a full sysroot later |
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
| File I/O | `open/read/write/lseek/fstat/stat/unlink/mkdir/opendir/readdir` | `write(1/2)` only (`heap.cc:279`); the VFS exists but there is no POSIX bridge |
| Memory | file-backed `mmap`, `mprotect`, `MAP_FIXED`, real `munmap` | anonymous `mmap` only; `munmap` is a no-op, `mprotect` absent (`heap.cc:215-274`) |
| Process | `posix_spawn`/`fork`/`execve` for cc1 + ld | none; Aegir spawn is a capability path (`specs/director.md`) |
| Threads | `pthread_create`, mutex, atomics | `clone` refused (`specs/cxx.md:214`); buildable with `LLVM_ENABLE_THREADS=OFF` |
| Signals | `sigaction` (crash handlers) | none; disable or stub |
| Time/env | `clock_gettime`, `nanosleep`, `getenv`, `uname`, `getcwd` | `argc`/`argv` only (`specs/environment.md`) |
| C++ | libc++ **no EH/RTTI** | already built tier-1 (`scripts/build_libcxx.sh:91-92`) |
| Assembler/linker | integrated assembler + lld | buildable from the vendored tree; not on device |

## The shape of the work

```
specs/clang-on-aegir.md                  # this file
scripts/build_llvm.sh                    # Phase 1: cross-build clang+lld+builtins
libs/aegir-llvm/imported.cmake           # Phase 1: expose clang/lld to CMake
third_party/patches/projects/llvm-project/0002-riscv64-aegir-triple.patch  # the one new LLVM patch
libs/aegir-posix/                        # Phase 2: the personality (files, mem, env, time)
libs/aegir-heap/src/heap.cc              # grows: the dispatcher gains the new handlers
apps/aegir-cc/                           # Phase 3: libclang + liblld single-process driver
apps/aegir-clang-test/                   # the acceptance service
```

### Phase 0 — this document, and the corrections it forces

This plan is the deliverable. Writing it also settles three housekeeping points:

- `specs/cxx.md:231` says "Clang 22" of the host cross-compile; the on-device
  compiler is the vendored **LLVM 18.1** (`manifests/aegir.xml:86`), and that
  note should say so.
- `THIRD-PARTY.md` does not list `projects/llvm-project/` at all, though the
  manifest pins it and the hosted runtime builds libc++ from it. A row is owed
  (Apache-2.0 with LLVM-exception), and clang/lld's role belongs in it.
- `specs/build.md`'s "Deferred: compiler choice" is about the *host* compiler;
  this document is a different decision and should be cross-referenced, not
  merged into it.

### Phase 1 — cross-build `clang` + `lld` (the first landed milestone)

New `scripts/build_llvm.sh`, modelled on `scripts/build_libcxx.sh`,
cross-compiling against the already-built `musl_full` and exposed by a new
`libs/aegir-llvm/imported.cmake`. The configuration, and why:

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
| `LLVM_APPEND_VC_REV` | `OFF` | hermeticity: the same `GIT_CEILING_DIRECTORIES` concern as `dtc` (`specs/build.md:221`) |

`compiler-rt`'s builtins for `riscv64` are built with the same cross toolchain;
they are freestanding and need no libc. libc++abi is already built by
`scripts/build_libcxx.sh`.

**Acceptance.** Both binaries exist, `readelf` reports a static `riscv64` ELF,
sizes are measured against the 32 MiB disk (below), and any warning from our own
patch is fixed (`AGENTS.md`).

### Phase 2 — the POSIX personality

A new `libs/aegir-posix`, with the handlers reached through the existing
`__sysinfo` switch, backed by Aegir's VFS, ports and seL4. Each sub-arc has its
own acceptance client, independent of clang, because the rule is that the calls
live in a library and not a program (`specs/userland.md:149-156`):

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
6. **Process** — `posix_spawn`/`execve` over Aegir's spawn. Deferred: Phase 3's
   single-process driver does not need it.

### Phase 3 — the on-device driver

`apps/aegir-cc` links libclang and liblld and does the whole job in one process:
build a `CompilerInstance` from `-cc1` options, emit an object with the
integrated assembler, then call lld in-process to link, then write the ELF.
No `fork`, no `exec`.

`apps/aegir-clang-test` is the acceptance service: it runs at boot, compiles a
known freestanding source file, links it with a tiny crt, spawns the result, and
prints the marker. The first compile is deliberately freestanding
(`-ffreestanding -nostdinc`) so that no on-device sysroot is needed yet; the
sysroot arc — musl headers, `libc.a`, `libc++.a`, and the `sel4runtime` crt —
follows and is what makes an ordinary Aegir C/C++ program compile.

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
  **32 MiB** (`scripts/make_disk.py:34`, four FAT partitions) and cannot hold it.
  The compiler gets a dedicated, larger image and target rather than growing the
  disk the existing tests stand on — the created-once, `-snapshot` invariant
  (`scripts/targets.py`) stays true for those.
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
- **Where the compiler lives and how it is reached.** Phase 3 runs it from a
  manifest service. A user reaching it interactively needs a shell or a session
  command, and neither exists yet (`specs/services.md`'s session row is a
  placeholder). That is a later arc.
- **On-device sysroot packaging.** What goes on the compiler volume, and how it
  is versioned with the runtime, is not designed here.
- **Whether `posix_spawn` is ever needed.** The in-process driver avoids it; the
  stock `clang` binary would not. This plan chooses the driver, but a POSIX
  process model may be wanted for other ports and is the natural next client of
  Phase 2.6.
- **Threads off, or on.** Building with `LLVM_ENABLE_THREADS=OFF` defers real
  threads to Phase 2.5. Whether Phase 3 should instead enable them depends on
  measured compile behavior under QEMU.
- **The LLVM revision.** 18.1 is the pinned tree and the default here. A bump is
  a `specs/third_party.md` upgrade exercise, not a subset of this plan.