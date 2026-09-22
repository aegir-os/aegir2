# Aegir OS

Aegir is a general purpose, multi-user operating system utilizing the seL4 microkernel.

It's meant to be significantly inspired by Amiga OS 3.1, and Workbench, brought into more modern times.

## Supported hardware

Aegir will support multiple platforms, specifically RISC-V 64 and AArch64. Extending to x86-64 is possible
in the future.

Initial target will be qemu virt machine on RISC-V 64 (qemu-system-riscv64).

The machine envelope Aegir designs for, which everything capacity-shaped has to
survive:

| | Floor | Expected |
| --- | --- | --- |
| RAM | **2 GiB** | 4-8 GiB |
| Cores | 1 | more on real boards; QEMU targets are 1, 2 and 4 |

The floor is what `make run` boots by default, deliberately: a system that only
works on a big machine has a capacity problem that nobody has met yet. The rest
of the matrix is data in `scripts/targets.py` and one `make envelope` away
(`specs/build.md`).

## Specifications

- `specs/director.md` — the root task: the services it starts, the authority it
  hands out, and how it spawns a process.
- `specs/services.md` — services, ports, the flat initrd and the boot manifest.
- `specs/authority.md` — identity, the two classes of authority, the right to
  spawn, and accounts.
- `specs/vfs.md` — the namespace, the volume protocol, and capabilities over
  IPC.
- `specs/namespace.md` — the union: a name read as an ordered list of
  directories.
- `specs/environment.md` — the process environment: arguments, environment
  variables, and the current directory.
- `specs/auth.md` — the user database's format and the login port's protocol.
- `specs/console.md` — the display, the pointer, the windows, and the greeter.
- `specs/trinket.md` — the GUI toolkit: console integration, the look, and the
  greeter rebuilt on it.
- `specs/bureau.md` — the session's desktop, and the screen-size query.
- `specs/workbench.md` — the screen title bar and the always-visible menus.
- `specs/window-manager.md` — client-side decorations, move/raise, and the
  titlebar drag.
- `specs/amiga-fidelity.md` — where the GUI is not Amiga, and the fixes.
- `specs/build.md` — build environment, targets, and what our image is made of.
- `specs/cxx.md` — the hosted C++ runtime: the vendored musl and libc++, the
  freeing heap, and the two build switches that gate them.
- `specs/third_party.md` — how third-party code is pinned, fetched and patched.
- `specs/userland.md` — what Aegir's userland is, and what it is not.
- `specs/clang-on-aegir.md` — the plan for an on-device LLVM/Clang: the POSIX
  personality it needs, the target triple, and the phases.

