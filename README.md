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
  Aegir userspace is just starting, with its boot, service and authority design
  specified in `specs/director.md`, `specs/services.md` and `specs/authority.md`.

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
make run-ui      # boot it with a GTK window on the displays; you press the keys
make envelope    # boot every machine in the RAM/cores envelope we support
make test        # build + boot the seL4 test suite (kernel acceptance test)
```

The hosted C++ runtime and the GUI toolkit are on by default, so `make run-ui`
shows the greeter's login form. `make run HOSTED_CXX=0` is the lean freestanding
build (the root task and services, with the greeter and bureau as placeholders),
and `TOOLKIT=0` keeps the hosted runtime but leaves the toolkit out. See
`specs/cxx.md`.

`make build` and `make run` act on `TARGET`, which defaults to `aegir` -- the
floor of the envelope (2 GiB, one core). `make run TARGET=aegir-8g-smp4` boots
the upper end; `scripts/targets.py` holds the list.

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

## AI Usage

This project was created with significant use of LLMs. Which model varied
based on task, and experimentation with many models. If you're curious,
you can see the contents of `specs/` to see a lot of the decisions that
went into what's here. Some of them were hand-written, some of them
are written by the LLM based on Plan mode conversations. And if you read
the AGENTS.md file, you can see some of the pain points that came up during
this project.

## Project "decrees"

1. No telemetry will ever be present in the OS or provided applications.
1. The OS and provided apps will never "phone home" - the only exception
   to this will be a (at some future date) system updater, and even then,
   it will be at the user's discretion when that occurs.
1. A LLM agent harness will never be provided as a part of the base,
   working distribution of the system. Developers can write or port their
   own as desired.

... this may be extended at any time ...

## Note from the author:

I understand, and agree with, many of the arguments against LLM usage. This
project isn't the place to discuss them, however.

The use of an LLM here helped me create something I've wanted for a long time,
but was always out of reach. I did this project because it was fun for me.
I hope you find it enjoyable to use.

Please don't file issues about the use of an LLM here. I'm not open to
a criticism of the LLM usage. I am open to bugs, new features, etc, like
any other open source project. If you do file issues that are critical of
the LLM usage, you'll be given one warning, the issue will be locked, and
closed.

[sel4]: https://sel4.systems/
