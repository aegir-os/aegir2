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

- Compiler: GCC for `riscv64-unknown-elf` (from the pinned container image),
  with `CROSS_COMPILER_PREFIX=riscv64-unknown-elf-` **pinned explicitly** in
  `configs/` so that `kernel/gcc.cmake`'s prefix probe can never silently pick
  up a different (e.g. `riscv64-unknown-linux-gnu-`) toolchain from `PATH`.
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

## Container (the supported build environment)

The host is not assumed to have cmake, ninja, dtc or a cross compiler. Builds
run in a podman image derived from
`seL4-CAmkES-L4v-dockerfiles`, pinned to:

- a fixed commit of that repository, and
- a fixed Debian snapshot (`USE_DEBIAN_SNAPSHOT=yes`, `SNAPSHOT_DATE=…`), so the
  package set cannot drift under us.

The image digest is recorded here when M1 lands. `scripts/container` wraps
`podman run` so `make` targets behave the same inside and outside the container
(workspace mounted, `--userns=keep-id` so files stay owned by the invoking
user).

The image also carries `repo` (pinned) so dependency sync can run there, and
`reuse`, which makes an SPDX bill of materials cheap if we ever want one.

## Host prerequisites

`podman`, `git`, `make` — that is all, for the containerised path. For the
on-host path (development convenience only, not supported for releases):
`cmake` ≥ 3.16, `ninja`, `device-tree-compiler`, `u-boot-tools`, `xxd`,
`libxml2-utils`, `cpio`, and the `riscv64-unknown-elf` GCC 14 toolchain.

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
