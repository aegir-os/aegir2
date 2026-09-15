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

# We run under QEMU, which supplies OpenSBI, so the elfloader image is what
# QEMU loads.
set(SIMULATION ON CACHE BOOL "Build for simulation")

# Development target: kernel debug syscalls (seL4_DebugPutChar, used by the
# hello root task) and kernel assertions.
set(KernelDebugBuild ON CACHE BOOL "Kernel debug build")

# A root task needs room for the DTB, timer and interrupt capabilities.
set(KernelRootCNodeSizeBits 13 CACHE INTERNAL "")
