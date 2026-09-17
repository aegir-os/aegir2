#
# Aegir: a general purpose OS on the seL4 microkernel.
#
# This is the top-level CMake project. seL4's build system is driven through
# cmake-tool (vendored at tools/seL4), which expects the manifest layout:
# kernel/, projects/*, tools/seL4. See specs/build.md for the environment and
# targets, and settings.cmake for how a target is selected.
#
# The imports below are what a seL4 application project has to do explicitly:
#   - sel4_import_kernel()  builds kernel.elf
#   - sel4_import_libsel4() generates the kernel interface headers and the
#     sel4_autoconf/sel4 targets that both the ELF loader and our apps link
#   - musllibc_setup_build_environment_with_sel4runtime() sets the freestanding
#     user-mode flags and imports musl and sel4runtime
# seL4_libs (libsel4utils and friends) is imported by components that use it, so
# it is deliberately absent until we need it.
#

cmake_minimum_required(VERSION 3.16.0)

include(settings.cmake)

project(aegir C CXX ASM)

find_package(seL4 REQUIRED)
find_package(elfloader-tool REQUIRED)
find_package(musllibc REQUIRED)
# util_libs is not optional: the ELF loader links libcpio from it (for the CPIO
# archive it embeds), so without this import the 'cpio' target does not exist and
# the loader cannot compile.
find_package(util_libs REQUIRED)

sel4_import_kernel()

# Order matters here, and this is the order upstream uses.
#
# The ELF loader is added BEFORE the user-mode environment is set up.
# musllibc_set_environment_flags() rewrites the global link rule to inject the
# user CRT objects (crt0.o, crti.o, and GCC's crtbegin.o/crtend.o) into every
# target created afterwards. The loader is not a user program: it has its own
# crt0.S and a linker script that discards .eh_frame, so those objects break its
# link ("__EH_FRAME_BEGIN__ ... defined in discarded section").
ApplyData61ElfLoaderSettings(${KernelPlatform} ${KernelSel4Arch})
elfloader_import_project()

# User-mode environment, then the user-mode libraries and our own code.
sel4_import_libsel4()
musllibc_setup_build_environment_with_sel4runtime()
util_libs_import_libraries()

# C++ policy shared by every Aegir target. Freestanding: no exceptions, no RTTI,
# no thread-safe statics, and no standard library (there is none for this
# target). Getting this wrong is silent, so it lives in one place.
add_library(aegir-cxx-policy INTERFACE)
target_compile_features(aegir-cxx-policy INTERFACE cxx_std_17)
target_compile_options(
  aegir-cxx-policy
  INTERFACE -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -Wextra -Werror
)

add_subdirectory(libs/aegir-runtime)
add_subdirectory(libs/aegir-devtree)
add_subdirectory(libs/aegir-descriptor)
add_subdirectory(libs/aegir-block)
add_subdirectory(libs/aegir-registry)
add_subdirectory(libs/aegir-namespace)
add_subdirectory(libs/aegir-mem)
add_subdirectory(libs/aegir-bootstrap)
add_subdirectory(libs/aegir-ipc)
add_subdirectory(libs/aegir-log)
add_subdirectory(libs/aegir-manifest)
add_subdirectory(libs/aegir-spawn)
# aegir-hello is director's smoke client, so it is built before the task that
# packs it into its initrd.
add_subdirectory(apps/aegir-device-manager)
add_subdirectory(apps/aegir-virtio-blk)
add_subdirectory(apps/aegir-partmgr)
add_subdirectory(apps/aegir-fs-fat)
add_subdirectory(apps/aegir-vfs)
add_subdirectory(apps/aegir-hello)
add_subdirectory(apps/aegir-logger)
add_subdirectory(apps/aegir-director)

if(SIMULATION)
  include(simulation)
  # No SetSimulationScriptProperty(MEM_SIZE ...) here on purpose. The device
  # tree is dumped from QEMU at *configure* time (QEMU_MEMORY, 3072 MB by
  # default) and tells the kernel how much RAM it may use; the simulate script's
  # own -m must agree with it. Overriding MEM_SIZE smaller -- as the ARM
  # platforms do, for their own reasons -- makes the kernel touch RAM that is
  # not there and abort in init_freemem with a store access fault.
  GenerateSimulateScript()
endif()
