# Development on Aegir

Status: plan, for review (2026-10). This fixes how the on-device compiler
(`specs/clang-on-aegir.md`) is shipped and reached — the `Sys:Development` tree
— and resolves that spec's "on-device sysroot packaging" open item.

## The tree

The machine's development environment is a directory on the system volume
(`Sys:`, the AEGIR BFS volume; `scripts/make_disk.py`):

| Path | What it holds |
| --- | --- |
| `Sys:Development/C` | the compiler: `aegir-cc`, the in-process clang + lld driver (`specs/clang-on-aegir.md` Phase 3) |
| `Sys:Development/Include` | the sysroot's headers — musl's and libc++'s, as this runtime ships them |
| `Sys:Development/Libs` | the sysroot's libraries and crt — `libc.a`, `libc++.a`, `libc++abi.a`, `libunwind.a`, compiler-rt's builtins, and `sel4runtime`'s crt objects |

A program compiles against a sysroot of `Sys:Development`: the driver puts
`Sys:Development/Include` on the search path and links `Sys:Development/Libs`,
which is what makes an ordinary `#include <stdio.h>` program compile on device —
`specs/clang-on-aegir.md`'s "a full sysroot later", here.

## It is a program on the volume, not a boot service

The compiler is read from `Sys:` at runtime and run the way any program is —
from the session (`Run Sys:Development/C/aegir-cc …`), or through a `cc` command
in `Sys:C` that fronts it (`specs/dos.md`). No part of it is in the boot initrd.

That is a hard constraint, not a preference. The ELF loader places the rootserver
just after its own image, so a rootserver whose initrd carried the compiler — 84
MB — could not be loaded: measured, the boot died with nothing after the OpenSBI
banner. A disk has no such constraint; the compiler sits on a volume and the boot
image stays small.

## The target and the disk

The compiler and its sysroot are on a **dedicated target** with its own, larger
disk, so the `aegir` target's disk — created once and run `-snapshot`
(`scripts/targets.py`) — is not grown. The disk builds its `Sys:` volume with the
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
`Sys:Development/C/aegir-cc`, and a `cc` command in `Sys:C` fronts it with the
sysroot flags so a caller writes `cc hello.c`.

The acceptance boots the dedicated target, runs the compiler from the session on
a known source, spawns the result, and checks a marker — the shape
`specs/clang-on-aegir.md` states once.

## Open

- **The partition's size.** `Sys:`'s size is computed from its tree
  (`scripts/make_disk.py`); the development tree makes it a few hundred
  megabytes, which is the dedicated disk's reason.
- **The sysroot's exact contents** follow from what a program links; the compiler
  spec owns the list and this one owns where it lives.
- **The first compile is freestanding.** The driver's first milestone compiles
  `-ffreestanding -nostdinc` (`specs/clang-on-aegir.md`); the sysroot is what
  makes the next one an ordinary program, and the sysroot arrives with this tree.
