# Aegir OS

Aegir is a general purpose, multi-user operating system utilizing the seL4 microkernel.

It's meant to be significantly inspired by Amiga OS 3.1, and Workbench, brought into more modern times.

## Supported hardware

Aegir will support multiple platforms, specifically RISC-V 64 and AArch64. Extending to x86-64 is possible
in the future.

Initial target will be qemu virt machine on RISC-V 64 (qemu-system-riscv64).

## Specifications

- `specs/director.md` — the root task: the services it starts, the authority it
  hands out, and how it spawns a process.
- `specs/services.md` — services, ports, the flat initrd and the boot manifest.
- `specs/authority.md` — identity, the two classes of authority, the right to
  spawn, and accounts.
- `specs/build.md` — build environment, targets, and what our image is made of.
- `specs/third_party.md` — how third-party code is pinned, fetched and patched.
- `specs/userland.md` — what Aegir's userland is, and what it is not.

