# Build environment and targets

Status: decided (2026-09).

## Targets

| Target | Arch | Platform | ABI | Status |
| --- | --- | --- | --- | --- |
| `riscv64-qemu-virt` | riscv64 | `qemu-riscv-virt` | `rv64imafdc_zicsr_zifencei` / `lp64d` | **primary**, first milestone |
| `aarch64-qemu-virt` | aarch64 | `qemu-virt-aarch64` | — | planned, not started |
| `x86_64` | x86-64 | — | — | possible later |

The ABI is hard-float `lp64d`: `KernelRiscvExtD` is enabled, so the kernel's FPU
context switching and every user binary use the D extension. Verified in the
pinned source rather than assumed:

- `kernel/src/arch/riscv/config.cmake` sets `_KernelRiscvExtD ON` by default
  (it is only forced off for `LLVM_TOOLCHAIN` + RV32), so `lp64d` **is** seL4's
  default for RISC-V, not a departure from it. What differs from the default is
  the *verified* configuration: `kernel/configs/include/RISCV64_verified_include.cmake`
  sets `KernelRiscvExtD OFF`. We therefore get the upstream-default ABI and give
  up the verified-configuration claim — a trade recorded here deliberately.
- The RISC-V image flow already assumes this: OpenSBI is built with
  `PLATFORM_RISCV_ABI=lp64d` by default (`cmake-tool/helpers/rootserver.cmake`),
  and the toolchain resolves `rv64imafdc_zicsr_zifencei`/`lp64d` to an installed
  multilib.
- We pin `KernelRiscvExtD` explicitly in `configs/` anyway, so the ABI cannot
  change under us through a default flip in a future seL4 release. Changing it
  later is a whole-system change (every binary, every library), which is why it
  is decided and written down now.

All architecture-specific detail lives in `configs/` and the CMake glue so that
application code stays portable across the targets above (project rule).

## Toolchain

One toolchain builds everything — kernel, `libsel4`, the seL4 libraries and
Aegir's own userland — because a single CMake build is configured from the
kernel's toolchain file and user targets inherit its flags.

- Compiler: **Debian's `riscv64-unknown-elf-gcc` 14.2.0+19**, unpacked from its
  packages into `third_party/toolchain/` (`manifests/toolchain.toml`), addressed
  through `CROSS_COMPILER_PREFIX=riscv64-unknown-elf-`, which is **always set
  explicitly** in `configs/`. Setting it explicitly means the build can never
  silently pick up a different toolchain from `PATH` — including the Linux
  multilib flavour, whose use with seL4's explicit `mabi` flags is a known link
  failure ("can't link double-float modules with soft-float modules").
- **Native TLS is a hard requirement of the toolchain.** User code is compiled
  with `-ftls-model=local-exec` and seL4's runtime is built around native
  `tp`-relative TLS (musl's thread pointer, `sel4runtime`'s `static_tls`). A
  toolchain configured with `--disable-tls` makes GCC emit *emulated* TLS
  (`__emutls_v.*`, `__emutls_get_address`) with no way to switch it off, and the
  resulting root task dies at startup with `vm fault on code at address 0`. That
  is why the toolchain is Debian's (built `--enable-tls`) and **not** xPack's
  `riscv-none-elf` (built `--disable-tls`), which is otherwise the more
  attractive package: newer GCC, one tarball, no unpacking. `make tools-check`
  compiles a `__thread` probe and fails if a toolchain ever regresses to
  emulated TLS, so this cannot silently come back.
- The toolchain needs its own runtime libraries: Debian's `cc1` links
  `libisl`/`libgmp`/`libmpfr`/`libmpc` shared, and a Fedora host has no
  `libisl.so.23` at all. Those packages are pinned and extracted alongside the
  compiler, and `scripts/env.sh` puts them on `LD_LIBRARY_PATH`, so the
  toolchain does not depend on the host's idea of those libraries.
- C library: **not** from the toolchain. Userland uses the vendored
  `projects/musllibc` (built by the same build) plus `projects/sel4runtime` for
  the entry point. The toolchain's own newlib/picolibc payload is irrelevant:
  seL4 does not use it.
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
| RISC-V cross GCC (+ its runtime libs) | `manifests/toolchain.toml` (Debian `riscv64-unknown-elf-gcc` 14.2.0+19 and four library packages, sha256 from Debian's signed index) | `third_party/toolchain/` |
| `cmake`, `ninja`, and seL4's Python dependencies | `manifests/tools-declared.txt` (direct pins) → `manifests/requirements-tools.txt` (generated, version + sha256 per artifact, installed with `pip --require-hashes`) | `third_party/tools/venv/` |
| `dtc` | `manifests/toolchain.toml` (kernel.org release tarball, sha256 from the project's signed `sha256sums.asc`) | `third_party/tools/bin/` |

`make tools` fetches all of it; `make tools-check` re-verifies every pin
offline. `. scripts/env.sh` puts them on `PATH` (and the toolchain's own runtime
libraries on `LD_LIBRARY_PATH`).

Two details worth knowing:

- **`dtc` is required, not optional.** seL4's `qemu-riscv-virt` platform
  generates its device tree at configure time: it dumps a DTB from QEMU,
  converts it to DTS (`kernel/src/plat/qemu-riscv-virt/config.cmake`), and later
  compiles DTS back to a DTB for the ELF loader
  (`cmake-tool/helpers/dts.cmake`). Both directions call `dtc`. It is built from
  source because the host has no `dtc` and no way to install one. The build is
  kept hermetic: `GIT_CEILING_DIRECTORIES` stops `git describe` inside dtc's
  Makefile from reaching Aegir's own repository, which would otherwise stamp our
  HEAD and dirty state into the binary.
- **Python dependencies are locked, not guessed.** The seL4 build imports
  `yaml`, `jsonschema`, `ply`, `Jinja2`, `pyfdt`, `lxml`, `pyelftools` and
  `libarchive`. Upstream ships a `sel4-deps` metapackage, but it is a superset
  that also pulls formatting and lint tooling we do not need, so we pin the
  actual set: `manifests/tools-declared.txt` records the direct pins and
  `scripts/lock_tools.py` resolves the closure and writes the hashed lock file.
  `make lock-tools` regenerates it when a version is deliberately bumped.

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

`make test` is the acceptance test for the vendored kernel: it drives the real
seL4 build system (kernel, ELF loader, OpenSBI, musllibc, sel4runtime, libsel4)
and boots the upstream suite, stopping QEMU once it prints
`All is well in the universe`. It passes when the vendored tree, the pinned
toolchain and the chosen ABI all work together.

Rules that apply to all of the above:

- Every long-running command — anything that boots QEMU or runs a test suite —
  is wrapped in `timeout` (project rule) so a wedged process is detected rather
  than hanging the session.
- Zero compiler warnings, for Aegir's own code and for any component we patch.
  Disabling a warning is not an acceptable fix.
- Warnings from unmodified upstream code are upstream's business; we do not
  patch them away, and we do not add flags that mask them.
