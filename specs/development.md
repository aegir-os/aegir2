# Development on Aegir

Status: plan, for review (2026-10). This fixes how the on-device compiler
(`specs/clang-on-aegir.md`) is shipped and reached — the `Sys:Development` tree
— and resolves that spec's "on-device sysroot packaging" open item.

## The tree

The machine's development environment is a directory on the system volume
(`Sys:`, the AEGIR BFS volume; `scripts/make_disk.py`):

| Path | What it holds |
| --- | --- |
| `Sys:Development/C` | the compiler: `clang` and `lld`, with `cc` a name for the driver (`specs/clang-on-aegir.md` Phase 3) |
| `Sys:Development/Include` | the sysroot's headers — musl's and libc++'s, as this runtime ships them |
| `Sys:Development/Libs` | the sysroot's libraries and crt — `libc.a`, `libc++.a`, `libc++abi.a`, `libunwind.a`, compiler-rt's builtins, and `sel4runtime`'s crt objects |

A program compiles against a sysroot of `Sys:Development`: the driver puts
`Sys:Development/Include` on the search path and links `Sys:Development/Libs`,
which is what makes an ordinary `#include <stdio.h>` program compile on device —
`specs/clang-on-aegir.md`'s "a full sysroot later", here.

## It is a program on the volume, not a boot service

The compiler is read from `Sys:` at runtime and run the way any program is —
the session runs `Sys:Development/C/clang …` (or its `cc` name) directly. It is a
development tool of the `Sys:Development` tree and is not a `Sys:C` command. No
part of it is in the boot initrd.

That is a hard constraint, not a preference. The ELF loader places the rootserver
just after its own image, so a rootserver whose initrd carried the compiler — 84
MB — could not be loaded: measured, the boot died with nothing after the OpenSBI
banner. A disk has no such constraint; the compiler sits on a volume and the boot
image stays small.

## The target and the disk

The compiler and its sysroot are on a **dedicated target** with its own, larger
disk, so the `aegir` target's disk — created once, and answered through the
overlay `scripts/run_target.py` makes for each run — is not grown. The disk builds its `Sys:` volume with the
development tree, as the AEGIR volume's other content is
(`scripts/make_disk.py`'s `AEGIR_BFS_TREE`, through `scripts/mkfs_bfs.py`), so
`Sys:Development` is there at boot. The default targets keep the disk they have.

`Sys:` is BFS (`specs/bfs.md`), which is why an 84 MB compiler is a plain file
and not a partition-flavor question.

## Versioning

The sysroot is copied from the runtime the target already builds —
`musl-install`, `cxx-install`, compiler-rt's builtins, `sel4runtime` — so the
compiler and the runtime it links against cannot drift: they are one build. A
marker in `Sys:Development` names it and the driver reports it.

## Invocation and the first acceptance

The compiler is invoked as a program on the volume: the session runs
`Sys:Development/C/clang` (or `cc`). The driver itself puts
`Sys:Development/Include` on the include path and links `Sys:Development/Libs`
(above), so a caller writes `Sys:Development/C/cc hello.c`.

**Where that stands.** The programs exist and are deployed. `apps/hosted/aegir-clang`
and `apps/hosted/aegir-lld` build clang's and lld's own sources and are linked by
Aegir's CMake (`specs/clang-on-aegir.md`'s Phase 1, landed — 97 MiB and 57 MiB,
both static `riscv64` ELFs), and `development_tree` copies them into
`Sys:Development/C` as `clang` and `cc` (the same program under the name this file
uses) and as `ld.lld` — the name lld asks for, since it takes its flavour from how
it is invoked. Both are invoked now, and clang compiles: see *How far it reaches*
below. The earlier note here — that clang "returns 1 in silence" — was the state
before the acceptance was typed step by step, and it is not the state any more.

**The acceptance is typed, step by step — it must not live in the session's startup.**
A key typed while `Sys:S/Shell-Startup` is running is lost: measured twice, an
`execute Sys:S/Development-Acceptance` cued on that script's own last line never reached
the shell, and the guest went on with the demo's gestures while the run waited out its
quiet timeout. So the startup keeps only what works from a script and is quick (the
POSIX clients and `aegir-big`), and the dev target types each acceptance action as its
own step, the way every DOS step already does — click the terminal first, then one
command, because a cue says a command started, not that the shell will receive the key:

| cue | typed |
| --- | --- |
| `demo: filtered A#\? 2` | `Sys:Development/C/cc --version` |
| `clang version 20` | `Sys:Development/C/ld.lld --version` |
| `LLD 20` | `execute Sys:S/Development-Acceptance` |
| `AEGIR_HELLO_OK` | the compiled program's own marker |

The first cue is a demo line, and one no other step uses: the demo is a separate,
step-paced process, so its lines cannot appear until the startup has finished and the
runner has paced it there. (`demo: opened AEGIR.TXT` was the obvious choice and is
already a step's cue — one step per cue, `AGENTS.md`.) `Sys:S/Development-Acceptance`
reports between its commands (`echo`, and a `date` either side of the compile), so each
stretch of silence is one command rather than the whole acceptance.

**The startup's order is deliberate.** The POSIX clients and `aegir-big` run first
and the big programs last: loading a 97 MiB or 57 MiB program spends the spawn
path's untyped, and nothing reclaims a departed command's yet, so a big program
placed before `aegir-big` makes its spawn fail with `spawn: FAIL no untyped for the
command's runtime` — the cap `aegir-big` exists to break. That is a workaround with
a reason, not a fix.

The sysroot is where that file says it is: `Include` and `Libs` are the tree's
shape and not yet its content, because the first compile is freestanding.

The acceptance boots the dedicated target, runs the compiler from the session on
a known source, spawns the result, and checks a marker — the shape
`specs/clang-on-aegir.md` states once.

**How far it reaches.** Measured on `aegir-8g-smp4`: the acceptance is typed into the
shell, `AEGIR_ACCEPTANCE_COMPILING` prints, and `AEGIR_ACCEPTANCE_COMPILED` prints —
**the device compiles the source**, clang's driver and its cc1 both running on the guest
with nothing of the host in the loop. What ends the run is one command's silence:
`AEGIR_ACCEPTANCE_LINKED` never arrives after `COMPILED`, so the stretch is lld loading
its 57 MiB and linking, which the runner's 300 s window cannot span.

That window is deliberately small and it stays small: a timer is a guess, while a guest
that reports is a fact — which is why `Sys:S/Development-Acceptance` reports between its
commands rather than asking for a longer wait.

**What the link does, measured with the dispatch trace.** It is not a hang: the trace
(`-DAEGIR_HEAP_TRACE`) shows the linker alive and allocating right up to the silence —
`mmap 0x422cc000 0x4000`, `mmap 0x422c3000 0x2000`, its own badge — and the runner's quiet
window is what ends the run. Two things make it *look* like nothing is happening:

- `link.sh` runs lld with `--verbose` (`scripts/run_target.py:737-746`), and lld writes that
  to **stderr**. The terminal mirrors a command's *stdout* to the serial — which is how
  every marker here is read — but not its stderr, so the one command that would narrate
  itself narrates into the grid and not into the log.
- The work itself is slow, and the reason is a number: the link reads ~100 MB of crt
  objects and the archive through the **file** path, whose reads are **page-sized**, while
  the *image* loader next door maps **mega pages** where the filesystem serves a large
  `read-frame` (`libs/freestanding/aegir-mem/src/child_vspace.cc:354-385`; ~29 calls for
  lld's 57 MiB). Thousands of frame-sized reads, each an IPC and a mapping, is where the
  minutes go.

So the next thing to make fast — a *speed* question, not a timeout one — is the file read
path's granularity, and second whether a command's stderr should reach the serial at all,
since a long command that narrates itself into the grid is one the acceptance cannot see.

**First piece done: the copy path's frames are mega pages.** The frame loop a `copy` runs
through (`libs/aegir-posix/src/files.cc`) moved data a 4 KiB page per `read_frame` call;
it now asks for a **mega** page (`seL4_RISCV_Mega_Page` with `seL4_LargePageBits` — the same
frame the image loader maps blocks with at
`libs/freestanding/aegir-mem/src/child_vspace.cc:370`), and falls back to a page for a
volume that refuses a large frame: a refused read consumes nothing, so the same position is
asked for again, and the frame is freed with the size it was taken at. Verified on
`aegir-8g-smp4`: the DOS acceptance's `copy` step still fires and the chain behind it still
runs (`dir`, `type`), so nothing regressed. It does not unblock the link — the link *reads*
its inputs and `read()` has no frame path of its own — which is the next piece.

**That piece, its shape named.** `read()` (`libs/aegir-posix/src/files.cc:982`) loops on
`volume.read_handle(handle, offset, count - total, bytes)` and lets the volume choose how
much comes back, so a program reading ~100 MB — which is what the linker does with its crt
objects and the archive — pays the volume's own chunking on every call. The shape to give it
is the one the *write* side already has, and the copy path just took: a frame of our own
(the write path's `ensure_write_frame` is the model — allocate, map for our own access, reuse
across calls), read a frame's worth per call through `read_frame` at **mega** granularity
where the volume serves it, copy that out to the caller's buffer, and fall back to a page —
and then to the unchanged inline path — for a volume that refuses. Nothing about it wants a
wider timeout: the guest reports between commands, and a command that is *fast* reports
sooner.

The write side in the same file is the model to copy, identifiers and all: `g_write_frame`
and `g_write_frame_map` — one frame, claimed lazily by `ensure_write_frame` and kept for the
process's life — taken with `g_allocator->alloc_page(account, &error)` and mapped for our own
access with `g_scratch->map(frame)`, so the bytes can go *in*. A read frame is the same thing
with the bytes coming *out*: take it at `seL4_RISCV_Mega_Page` with `seL4_LargePageBits` (the
copy path above shows the page fallback), reuse it across calls rather than per call, and
`memcpy` each filled frame out to the caller's buffer.

Beside the compiler, the same session runs `aegir-big`: a command whose loaded
segment is a generated blob tens of megabytes long (`scripts/gen_blob.py`). It
is the scale acceptance for the spawn path (`specs/memory.md`) — a program
chosen to break the caps that have bounded one before (the retype fan-out, a
single-CNode pool, one frame mapped per 4 KiB page) — and its line is checked by
name, so a cap that creeps back fails the run rather than waiting to be found
when something larger is tried.

## Open

- **The partition's size.** `Sys:`'s size is computed from its tree
  (`scripts/make_disk.py`); the development tree makes it a few hundred
  megabytes, which is the dedicated disk's reason.
- **The sysroot's exact contents** follow from what a program links; the compiler
  spec owns the list and this one owns where it lives.
- **The first compile is freestanding.** The driver's first milestone compiles
  `-ffreestanding -nostdinc` (`specs/clang-on-aegir.md`); the sysroot is what
  makes the next one an ordinary program, and the sysroot arrives with this tree.
