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

#### Hard-float is a kernel capability, not only an ABI

`lp64d` says how floating-point arguments travel; whether a program may compute
in floating point at all is the kernel's decision, and it is made in three
places:

- `KernelRiscvExtD` → `CONFIG_HAVE_FPU` in the built kernel
  (`out/aegir/kernel/gen_config/kernel/gen_config.h`). With it the kernel saves
  and restores FP state per thread, lazily: the state is written out when the FPU
  is taken away, not on every switch
  (`kernel/src/arch/riscv/machine/fpu.c`, `lazyFPURestore` in
  `kernel/src/object/tcb.c:795-810`).
- A freshly retyped TCB has FP **enabled**: the only thread that opts out is the
  idle thread, which must not leave the FPU's state dirty
  (`kernel/src/kernel/thread.c:33`, `configureIdleThread`). This is why a service
  director creates can compute in floating point without anyone asking for it.
- A thread can opt *out* with `seL4_TCB_SetFlags(tcb, seL4_TCBFlag_fpuDisabled,
  0)` (`kernel/libsel4/include/sel4/constants.h:73-81`) to save the switching
  cost. Nothing in Aegir does that yet; it is the shape a per-service choice would
  take when a service that never touches a float is worth optimising.

The consequence for the trade above: `KernelRiscvExtD OFF` would not merely change
the ABI, it would take floating point away from every program. And it is checked
at boot rather than trusted — `apps/aegir-hello` computes in `float` and `double`
and reports `floating point: works`, or fails and says so.

All architecture-specific detail lives in `configs/` and the CMake glue so that
application code stays portable across the targets above (project rule).

### The machine, not the architecture

Two of the values a target fixes are not architecture at all -- RAM and cores --
and both are **configure-time**. For `qemu-riscv-virt` the platform config runs
QEMU once with `-m` and `-smp` to dump the device tree
(`kernel/src/plat/qemu-riscv-virt/config.cmake:83-132`), and the kernel, the ELF
loader and the image flow all read that DTB. A different RAM size or core count is
therefore a different build directory rather than a run-time flag, and the
simulate side has to offer QEMU the same number of harts or the kernel looks for
cores that are not there.

The envelope Aegir designs to is in `specs/aegir.md`: 2 GiB and one core at the
floor, 4-8 GiB and more cores expected. The QEMU matrix we build and boot:

| Target | RAM | Cores |
| --- | --- | --- |
| `aegir` -- the default, and the floor | 2048 MiB | 1 |
| `aegir-2g-smp2` | 2048 MiB | 2 |
| `aegir-2g-smp4` | 2048 MiB | 4 |
| `aegir-8g-smp4` | 8192 MiB | 4 |

`make envelope` builds and boots all four, because they are four kernels;
`make build` and `make run` act on `$(TARGET)`, which defaults to `aegir`. The
numbers are data -- defaults in `configs/riscv64-qemu-virt.cmake`, overridden per
target in `scripts/targets.py`, which also carries the `-smp` the simulate script
has no concept of -- so a new point in the matrix is an entry, not a code change.

SMP is a deliberate step further from the verified configuration: more than one
core sets `KernelMaxNumNodes`, which turns on `ENABLE_SMP_SUPPORT`
(`kernel/config.cmake:150-157`), and the configuration seL4 verifies for RISC-V is
single-core. The verification claim was already traded away for the default
`lp64d` ABI earlier in this file; this is the same kind of trade, recorded rather
than assumed. What the kernel guarantees is that the other harts come up and
idle. *Placing* work on them is userland's -- `seL4_TCB_SetAffinity`
(`out/aegir/libsel4/include/interfaces/sel4_client.h:1727`) -- and director does
not do that yet (`specs/authority.md`).

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

  Establishing this cost four separate discoveries, all now encoded in the
  build rather than in anyone's memory:

  1. There is no `<cstdint>` and no other C++ header: user code is compiled
     `-nostdinc -nostdinc++`. musl's staged include directory *is* on the
     include path, so C++ uses `<stdint.h>` — the C++-on-seL4 convention.
  2. Static constructors **do** run before `main`: `sel4runtime` walks
     `__preinit_array`/`__init_array` (`projects/sel4runtime/src/init.c`, called
     from `env.c`). `apps/aegir-hello` asserts this at boot so a regression
     cannot pass unnoticed.
  3. `sel4/assert.h` declares `__assert_fail` **without `extern "C"`**, which is
     fine in C (musl's C symbol matches) and a mangled, undefined reference in
     C++. `libs/aegir-runtime` provides the C++-linkage definition, and treats a
     failed assertion as what it is in Aegir: a fatal fault that reports and
     stops.
  4. A virtual destructor emits a deleting destructor, hence a reference to
     `operator delete`, which does not exist here. There is **no `operator
     new`/`delete` yet, deliberately**: Aegir gets an allocation story when it
     gets a memory story. Until then, classes with virtual destructors cannot be
     destroyed — cheap to avoid, expensive to discover late.

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
`wget` that the upstream image uses for it). It is also the environment upstream
tests seL4 in, which is the point of using it.

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
from the upstream image, and it is Debian's `riscv64-unknown-elf-gcc`
14.2.0+19. Anything the image's own compiler carries — its patch level, its TLS
configuration, the libraries it was linked against — is therefore not what we
build with, which is exactly what the `sel4test` acceptance run in M3 is there
to catch.

## Host prerequisites

`git`, `make`, `python3`, `curl` or `wget`, plus a C toolchain for anything
built from source. Everything else is fetched on demand:

- Fetched and pinned by `make tools`: `cmake`, `ninja`, the RISC-V cross GCC,
  and `device-tree-compiler` (`dtc`) — required, not optional, since the
  platform flow calls it in both directions, and built from source because the
  host has neither `dtc` nor a way to install one.
- Provided by the host and used directly: `cpio`, `xxd`, `xmllint`, `flex`,
  `bison`, `python3`, `make`, `git`, `repo`, `qemu-system-riscv64`. `repo` is
  the launcher only; the tool code it downloads is pinned (see
  `specs/third_party.md`).
- Known gaps, to be resolved only if a build actually needs them: `patch`,
  `gperf`. Our own patch application uses `git apply`, so `patch` is not
  required for that.

## Commands

```sh
make deps         # fetch vendored sources at their pinned revisions
make deps-check   # verify pins, patches and license files
make build        # configure + build $(TARGET) (default: aegir)
make run          # boot $(TARGET) under QEMU
make envelope     # build + boot every machine in the envelope above
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

## Building our own image

`make build` configures and builds Aegir's root task; `make run` boots it and
stops QEMU once it prints its marker. The top-level `CMakeLists.txt` follows the
shape of a seL4 application project, and three rules there are load-bearing:

- **`util_libs` is not optional.** The ELF loader links libcpio from it for the
  CPIO archive it embeds. Without the import, no `cpio` target exists and the
  loader silently links `-lcpio` instead, then fails to compile.
- **The ELF loader is imported before the user-mode environment is set up.**
  `musllibc_set_environment_flags()` rewrites the global link rule to inject the
  user CRT objects (crt0.o, crti.o, GCC's crtbegin.o/crtend.o) into every target
  created afterwards. The loader is not a user program — it has its own crt0.S
  and a linker script that discards `.eh_frame` — so injected CRT objects break
  it ("`__EH_FRAME_BEGIN__` ... defined in discarded section").
- **The simulate script's `-m` must agree with the device tree.** The DTB is
  dumped from QEMU at *configure* time (`QEMU_MEMORY`, 3072 MB by default) and
  tells the kernel how much RAM it may use. Overriding the simulate script's
  `MEM_SIZE` smaller — as the ARM platforms do — makes the kernel touch RAM that
  is not there and abort in `init_freemem` with a store access fault. We
  therefore do not override it.

Targets are added by dropping a file in `configs/` (see `settings.cmake`), so a
new board or architecture is data, not a rewrite — per the project rule that
architecture-specific code stays abstracted.

Every target is configured and booted through `scripts/run_target.py`, which
streams the guest console and stops QEMU when the target's success marker
appears (QEMU never exits on its own). `scripts/targets.py` holds the target
list.

## The root task, and what QEMU actually loads

Three things make `apps/aegir-director` the root task rather than an ordinary
program (it took over from `apps/aegir-hello` in M6; see the end of this
section):

- Its `CMakeLists.txt` includes seL4's `rootserver` module and calls
  `DeclareRootserver(aegir-director)`. That sets the entry point to `_sel4_start`
  (`-Wl,-u_sel4_start -Wl,-e_sel4_start`) and links the target with
  `cmake-tool/helpers/tls_rootserver.lds` — the script that lays out a root
  task's `.tdata`/`.tbss`, which is what `tp`-relative TLS needs.
- It links `aegir-runtime`, `aegir-mem`, `aegir-manifest`, `libsel4`,
  `sel4runtime`, musl's `libc.a` and util_libs' `cpio` (which reads its initrd),
  and no `libsel4muslcsys` at all (see `specs/userland.md`).
- The ELF loader embeds it: `elfloader-tool/CMakeLists.txt` strips the kernel
  and the root task and packs them into a CPIO archive (`MakeCPIO(...)`, symbol
  `_archive_start`) inside the loader's own image. For this target the archive
  holds exactly `kernel.elf`, `kernel.dtb` and `rootserver`, and the loader's
  console reports finding it.

The file QEMU is handed is **not** the ELF loader. On RISC-V the image flow
(`cmake-tool/helpers/rootserver.cmake`, `UseRiscVOpenSBI`) objcopies the ELF
loader to a flat binary and builds the vendored `tools/opensbi` with it as
`FW_PAYLOAD_PATH`, so
`images/aegir-director-image-riscv-qemu-riscv-virt` is OpenSBI's `fw_payload.elf`
with the loader as its payload. The simulate script passes `-bios none`: QEMU
supplies no firmware, and the banner on the console is the pinned OpenSBI
(v0.9, the revision the seL4 16.0.0 release manifest picks).

A green boot, from `make run`:

```text
OpenSBI v0.9 ... Firmware Base : 0x80000000, Firmware Size : 100 KB
ELF-loader started on (HART 0) (NODES 1)
Looking for DTB in CPIO archive...found at 810204e8.
Loaded DTB from 810204e8.
ELF-loading image 'kernel' to 80200000
ELF-loading image 'rootserver' to 80223000
Enabling MMU and paging / Jumping to kernel-image entry point...
Booting all finished, dropped to user space
Aegir: director online
the machine
  cnode size: 2^13 slots; 8055 free for us
  untyped caps: 60; ipc buffer at 0x49000
memory
  untyped: 60 caps, 2 GiB normal, 508 GiB device
  self test: retyped, mapped, wrote and read back a page
  window: 0x4d000..0x200000, 4 KiB mapped
  charged to `system`: 4 KiB in 1 objects
initrd (flat: names are identities, there are no paths)
  services.manifest  986 bytes
  aegir-hello  53944 bytes
boot manifest
  1 service(s) declared
  hello: binary aegir-hello, authority system, account boot
AEGIR_BOOT_OK
```

`AEGIR_BOOT_OK` is the `aegir` target's marker in `scripts/targets.py`.

How a root task is declared, packaged and loaded — everything above — stays true
whatever the root task binary is. Which binary that is changed in M6: the root
task is `apps/aegir-director` (`specs/director.md`), and M4's root task,
`apps/aegir-hello`, is now packed into director's own initrd as the client it
starts first. Director also carries a second, Aegir-owned archive — the initrd
above, holding the service binaries and the boot manifest — which is a different
archive from the loader's, and the one `specs/services.md` describes as the flat
filesystem that will mount as `Initrd:`.
