#
# Target configuration: RISC-V 64 on QEMU's virt machine.
#
# This file is data, not code: the arch matrix lives here so that porting Aegir
# to another board or architecture is a new file rather than a rewrite. See
# specs/build.md for the rationale behind each value.
#

# seL4 platform/arch selection.
set(PLATFORM "qemu-riscv-virt" CACHE STRING "seL4 platform")
set(KernelSel4Arch "riscv64" CACHE STRING "seL4 architecture")

# Hard-float lp64d (rv64imafdc_zicsr_zifencei/lp64d): kernel FPU context
# switching and every user binary use the D extension. This is seL4's default
# for RISC-V; we pin it so a future upstream default flip cannot change our ABI
# silently. The ABI is a whole-system property -- changing it later changes
# every binary -- so it is recorded here and in specs/build.md.
set(KernelRiscvExtD ON CACHE BOOL "RISC-V double-precision floating point")

# The clang target triple. Setting TRIPLE is what makes seL4's seL4Config.cmake
# choose kernel/llvm.cmake over gcc.cmake
# (kernel/configs/seL4Config.cmake:244-248), so this one value switches the
# build from GCC to clang (specs/build.md). The host clang is the pinned LLVM
# release in manifests/toolchain.toml's `[llvm]` section.
set(TRIPLE "riscv64-unknown-elf" CACHE STRING "clang --target triple; selects seL4's llvm.cmake")

# The GNU cross prefix, kept for OpenSBI only: seL4's RISC-V image flow builds
# OpenSBI with `${CROSS_COMPILER_PREFIX}gcc` and probes its version
# (tools/seL4/cmake-tool/helpers/rootserver.cmake:83-97,126), and nothing else
# in Aegir uses GCC. llvm.cmake derives the same prefix from TRIPLE, so the two
# agree.
set(CROSS_COMPILER_PREFIX "riscv64-unknown-elf-" CACHE STRING "GNU cross prefix (OpenSBI only)")

# The image flow's binutils resolve to LLVM's, not the host's x86 GNU ones: with
# a clang toolchain CMake would otherwise fall back to the host `objcopy`/
# `readelf`. They come from the same pinned LLVM release, on PATH via
# scripts/env.sh; `find_program` records absolute paths in the cache.
find_program(CMAKE_OBJCOPY llvm-objcopy REQUIRED)
find_program(CMAKE_READELF llvm-readelf REQUIRED)
find_program(CMAKE_AR llvm-ar REQUIRED)
find_program(CMAKE_RANLIB llvm-ranlib REQUIRED)
find_program(CMAKE_NM llvm-nm REQUIRED)
find_program(CMAKE_STRIP llvm-strip REQUIRED)

# The RISC-V ISA and ABI flags every object needs. GCC's Debian multilib made
# rv64imafdc/lp64d the *default* for riscv64-unknown-elf, so seL4's user-mode
# build never had to state it; clang's default is soft-float, so an object
# compiled without these -- libsel4's, for one -- will not link against the
# double-float crt ("cannot link object files with different floating-point
# ABI"). Stated in the base flags, the kernel, libsel4, muslc and every Aegir
# target agree. FORCE, because the ABI is a whole-system property (specs/build.md)
# and a stale cache value silently produced a soft-float build.
set(CMAKE_C_FLAGS "-march=rv64imafdc_zicsr_zifencei -mabi=lp64d" CACHE STRING "RISC-V ISA and ABI" FORCE)
set(CMAKE_CXX_FLAGS "-march=rv64imafdc_zicsr_zifencei -mabi=lp64d" CACHE STRING "RISC-V ISA and ABI" FORCE)
set(CMAKE_ASM_FLAGS "-march=rv64imafdc_zicsr_zifencei -mabi=lp64d" CACHE STRING "RISC-V ISA and ABI" FORCE)

# Simulation build. The image QEMU is handed is not the bare ELF loader: the
# RISC-V image flow builds the vendored OpenSBI with the loader as its payload
# (see specs/build.md), and the simulate script runs QEMU with -bios none.
set(SIMULATION ON CACHE BOOL "Build for simulation")

# The machine this build is for: RAM and cores. Both are baked in at *configure*
# time, because QEMU is run once with `-m` and `-smp` to dump the device tree
# that the kernel, the ELF loader and the image flow all read
# (kernel/src/plat/qemu-riscv-virt/config.cmake:83-132). A change here is
# therefore a new target and a new build directory, not a run-time flag
# (scripts/targets.py). The values below are the floor of the envelope Aegir
# designs to -- 2 GiB and one core (specs/aegir.md) -- and a target that widens
# it passes -DQEMU_MEMORY=... / -DKernelMaxNumNodes=... on the configure line,
# which wins: the platform config only sets these when they are not already
# defined, and these are plain CACHE sets without FORCE.
set(QEMU_MEMORY "2048" CACHE STRING "RAM the kernel is built for, in MiB")
# More than one turns on ENABLE_SMP_SUPPORT (kernel/config.cmake:153-157) and
# dumps the device tree with `-smp N`, so the run side has to offer QEMU the same
# number of harts or the kernel looks for cores that are not there.
set(KernelMaxNumNodes 1 CACHE STRING "CPU cores the kernel is built for")

# Development target: kernel debug syscalls (seL4_DebugPutChar, used by the
# hello root task) and kernel assertions.
set(KernelDebugBuild ON CACHE BOOL "Kernel debug build")

# Root CNode size, as 2^16 slots. Upstream's sel4test project sets 13 for its
# root task and notes that it is "large enough for DTB, timer caps, etc" but
# "may need to be increased in the future"
# (projects/sel4test/CMakeLists.txt). The root task's own capability use needs
# more: director gives each spawning child a copy of the initrd, one frame cap
# per page (aegir-spawn's process.cc -- a frame cap remembers the one address
# space it is mapped in, so a second child gets a copy), and the hosted greeter
# and bureau carry their debug sections into the initrd. At 13 (8192 slots) the
# initrd's pages alone filled the CNode; 16 is the kernel's own upstream
# default, and the comment above says to raise it when the root task needs it.
# The cost is a megabyte of capability memory, which the floor has.
set(KernelRootCNodeSizeBits 16 CACHE INTERNAL "")
