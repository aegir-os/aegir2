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
| `apps/` | Aegir programs — root task, later servers and drivers |
| `libs/` | Aegir libraries (runtime pieces every binary links) |
| `configs/` | Per-target build configuration (platform, arch, ABI) |
| `manifests/` | `repo` manifests: the vendoring pins and their upstream source |
| `specs/` | Decided specifications |
| `scripts/` | Host tooling: dependency sync, patch application, pin checks |
| `third_party/patches/` | Our patches to vendored components (tracked) |

Vendored trees (`kernel/`, `projects/*`, `tools/seL4|opensbi|nanopb`) are
extracted into the paths the seL4 build system expects and are gitignored.

## Getting started

```sh
make tools       # fetch the pinned toolchain and host build tools (no root)
make deps        # fetch the vendored seL4 tree at its pinned revisions
make deps-check  # verify every vendored tree matches its pin
make build       # configure + build Aegir's root task
make run         # boot it under QEMU (stops once it reports online)
make test        # build + boot the seL4 test suite (kernel acceptance test)
```

`make tools` and `make deps` need network access and happen once; after that the
build is offline. Every step is pinned: revisions in `manifests/`, tool versions
by version and hash, and nothing is installed outside the repository.

Host prerequisites and the toolchain story are in `specs/build.md`. The
environment is workspace-local and pinned: nothing is installed system-wide and
no root is required. A container derived from seL4's CI image is the intended
environment for CI and release builds, but it cannot be used in this
development sandbox — see `specs/build.md`.

## License

Aegir's own code is MIT (`LICENSE`). Vendored components keep their own
licenses — see `THIRD-PARTY.md`.

[sel4]: https://sel4.systems/
