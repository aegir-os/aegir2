# Aegir

A general purpose, multi-user operating system built on the [seL4][sel4]
microkernel, inspired by Amiga OS 3.1 and Workbench.

- **Microkernel:** seL4 16.0.0, pinned by SHA and fetched on demand (never
  committed) — see `specs/third_party.md`.
- **First target:** RISC-V 64 (`riscv64`, hard-float `lp64d`) on the QEMU
  `virt` machine. AArch64 is planned; the build is kept architecture-neutral
  (`specs/build.md`).
- **Languages:** C++ primarily, C where the platform demands it.
- **Status:** pre-alpha. The kernel is vendored and validated end-to-end; the
  Aegir userspace is just starting.

Aegir is not a POSIX system and does not intend to be one; a POSIX
compatibility layer may be added later as a user-level service
(`specs/userland.md`).

## Layout

| Path | Contents |
| --- | --- |
| `apps/` | Aegir programs (root task, servers, drivers) |
| `configs/` | Per-target build configuration (platform, arch, ABI) |
| `manifests/` | `repo` manifests: the vendoring pins and their upstream source |
| `specs/` | Decided specifications |
| `scripts/` | Host tooling: dependency sync, patch application, pin checks |
| `third_party/patches/` | Our patches to vendored components (tracked) |

Vendored trees (`kernel/`, `projects/*`, `tools/seL4|opensbi|nanopb`) are
extracted into the paths the seL4 build system expects and are gitignored.

## Getting started

```sh
make deps        # fetch pinned third-party sources (network, ~minutes)
make deps-check  # verify every vendored tree matches its pin
make build       # configure + build for the default target
make run         # boot under QEMU (always wrapped in timeout)
```

Host prerequisites and the toolchain story are in `specs/build.md`. Everything
is built inside a pinned container image so the host stays untouched.

## License

Aegir's own code is MIT (`LICENSE`). Vendored components keep their own
licenses — see `THIRD-PARTY.md`.

[sel4]: https://sel4.systems/
