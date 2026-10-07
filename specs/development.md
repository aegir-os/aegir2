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
it is invoked. The session's startup runs `ld.lld --version`, which answers on the
guest with its own banner and is the dev target's cue; clang runs there and returns
1 in silence, which is Phase 3's open item (`specs/clang-on-aegir.md`), so its
invocation waits while its deployment does not.

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
