# Build environment and targets

Status: decided (2026-09).

## Targets

| Target | Arch | Platform | ABI | Status |
| --- | --- | --- | --- | --- |
| `riscv64-qemu-virt` | riscv64 | `qemu-riscv-virt` | `rv64imafdc_zicsr_zifencei` / `lp64d` | **primary**, first milestone |
| `aarch64-qemu-virt` | aarch64 | `qemu-virt-aarch64` | — | planned, not started |
| `x86_64` | x86-64 | — | — | possible later |

Hard-float `lp64d` is a deliberate departure from seL4's default (soft-float
`lp64`): it enables `KernelRiscvExtD`, so both the kernel's FPU context
switching and every user binary use the D extension. This matches the ABI the
RISC-V image flow and OpenSBI already default to, and it is a whole-system
decision that is painful to change once Aegir binaries and ABIs exist — hence
decided up front and recorded here.

All architecture-specific detail lives in `configs/` and the CMake glue so that
application code stays portable across the targets above (project rule).

## Toolchain

One toolchain builds everything — kernel, `libsel4`, the seL4 libraries and
Aegir's own userland — because a single CMake build is configured from the
kernel's toolchain file and user targets inherit its flags:

- Compiler: the pinned xPack `riscv-none-elf-gcc` 15.2.0-1 toolchain
  (`manifests/toolchain.toml`), addressed through
  `CROSS_COMPILER_PREFIX=riscv-none-elf-`, which is **always set explicitly** in
  `configs/`. This is required, not merely tidy: seL4's `gcc.cmake` probes a
  list of known prefixes that does not include `riscv-none-elf-`, so an unset
  prefix is a hard configure error, and setting it also means the probe can
  never silently pick up a different (e.g. `riscv64-unknown-linux-gnu-`)
  toolchain from `PATH`.
  Mixing a Linux multilib toolchain with seL4's explicit `mabi` flags is a known
  link failure ("can't link double-float modules with soft-float modules"), so
  no second toolchain is ever added to `PATH`.
- C library: **not** from the toolchain. Userland uses the vendored
  `projects/musllibc` (built by the same build) plus `projects/sel4runtime` for
  the entry point. The image's `riscv64-unknown-elf` package ships no
  newlib/picolibc, which is irrelevant: seL4 does not use them.
- C++: the C++ frontend works, but there is no target `libstdc++`/`libc++`.
  C++ code is therefore **freestanding**: compiled with `-fno-exceptions
  -fno-rtti -fno-threadsafe-statics` and without the standard library. The
  standard-library question is a separate, deferred milestone; it is orthogonal
  to the compiler choice below.

### Deferred: compiler choice

GCC is used for this milestone set. Modern LLVM was considered and deferred
deliberately, decided after vendoring is validated. Facts recorded so the
deferred decision is cheap:

- seL4 16.0.0 ships `kernel/llvm.cmake`, a first-class LLVM toolchain file
  (`LLVM_TOOLCHAIN ON`, `clang`/`clang++` with `--target=${TRIPLE}`), and the
  kernel source contains clang-specific handling (including clang ≥ 20).
- One CMake build cannot mix compilers per target, so "GCC for the kernel,
  clang for Aegir's userland" is only realisable as a hybrid build (packaging
  our clang-built ELFs into the image via the cmake-tool, which supports it:
  `MakeCPIO` takes file paths and `DeclareRootserver` marks the root task) or as
  two builds with a standalone kernel artifact.
- Even a clang-everywhere build needs the GNU cross toolchain: the RISC-V image
  flow builds OpenSBI with `${CROSS_COMPILER_PREFIX}gcc` and probes
  `${CROSS_COMPILER_PREFIX}gcc -dumpversion`
  (`cmake-tool/helpers/rootserver.cmake`), and GNU binutils supply
  `objcopy`/`readelf`.
- Our `lp64d` ABI already matches the container's existing clang `libgcc.a`
  wiring (`rv64imafdc/lp64d`), so the clang path would need no extra plumbing.

## Build environment

The environment is **workspace-local and pinned**; nothing is installed
system-wide and no root is required.

| Input | Pin | Where it lands |
| --- | --- | --- |
| RISC-V cross GCC | `manifests/toolchain.toml` (xPack `riscv-none-elf-gcc` 15.2.0-1, sha256) | `third_party/toolchain/` |
| `cmake`, `ninja` | `manifests/requirements-tools.txt` (version + wheel sha256, installed with `pip --require-hashes`) | `third_party/tools/venv/` |

`make tools` fetches both; `make tools-check` re-verifies them against their
pins (including that the toolchain really carries the `rv64imafdc/lp64d`
multilib our ABI needs). `. scripts/env.sh` puts them on `PATH`.

### Containers: preferred, but not in the development sandbox

The recommended environment for CI and release builds is a container derived
from `seL4-CAmkES-L4v-dockerfiles` (pinned commit + `USE_DEBIAN_SNAPSHOT=yes`
with a fixed `SNAPSHOT_DATE`, and a pinned `repo` instead of the unverified
`wget` that the upstream image uses for it). That is also what reproduces the
toolchain upstream tests against.

It cannot be used in this development sandbox, for a reason worth recording so
nobody re-litigates it:

- rootless podman needs the setuid helper `newuidmap` to write `uid_map`, and
- the sandbox runs with `NoNewPrivs: 1` and **all capability sets empty**
  (`CapEff`/`CapPrm`/`CapBnd` = 0), so setuid helpers cannot elevate and `sudo`
  cannot run at all.

Namespace support itself is fine (`unshare -Urm` works); it is the setuid step
that is impossible. A container definition will be committed once it can be
built and tested somewhere real — not before, since untested build
configuration is worse than none.

Consequence: the *toolchain revision* is pinned by us rather than inherited
from the upstream image, and the compiler is GCC 15.2 rather than the image's
GCC 14.2. That is a wider gap from what upstream CI exercises, which is exactly
what the `sel4test` acceptance run in M3 is there to catch.

## Host prerequisites

`git`, `make`, `python3`, `curl` or `wget`, plus a C toolchain for anything
built from source. Everything else is fetched on demand:

- Provided by the host and used directly: `cpio`, `xxd`, `xmllint`, `flex`,
  `bison`, `python3`, `make`, `git`, `repo`, `qemu-system-riscv64`.
- Fetched and pinned by `make tools`: `cmake`, `ninja`, the RISC-V cross GCC.
- Known gaps, to be resolved only if a build actually needs them:
  `device-tree-compiler` (`dtc`), `patch`, `gperf`. Our own patch application
  uses `git apply`, so `patch` is not required for that.

## Commands

```sh
make deps         # fetch vendored sources at their pinned revisions
make deps-check   # verify pins, patches and license files
make build        # configure + build the default target
make run          # boot the image under QEMU
make test         # build + boot sel4test, check for the success marker
make clean
```

Rules that apply to all of the above:

- Every long-running command — anything that boots QEMU or runs a test suite —
  is wrapped in `timeout` (project rule) so a wedged process is detected rather
  than hanging the session.
- Zero compiler warnings, for Aegir's own code and for any component we patch.
  Disabling a warning is not an acceptable fix.
- Warnings from unmodified upstream code are upstream's business; we do not
  patch them away, and we do not add flags that mask them.
