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

# The toolchain prefix is set explicitly rather than probed. seL4's gcc.cmake
# probes a list of known prefixes, and relying on that would let an unrelated
# toolchain on PATH (a Linux multilib one, say) silently become our compiler.
set(CROSS_COMPILER_PREFIX "riscv64-unknown-elf-" CACHE STRING "Cross compiler prefix")

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

# Root CNode size, as 2^13 slots. The value follows upstream's sel4test
# project, which sets 13 for its root task and notes that it is "large enough
# for DTB, timer caps, etc" but "may need to be increased in the future"
# (projects/sel4test/CMakeLists.txt). It is headroom, not a requirement: the
# kernel's own default is 12, and this root task boots and reaches its marker
# with 12 as well. Raise it when the root task's own capability use needs more.
set(KernelRootCNodeSizeBits 13 CACHE INTERNAL "")
