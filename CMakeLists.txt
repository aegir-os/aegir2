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

# The hosted C++ runtime -- vendored full musl, libc++/libcxxabi/libunwind, and
# the acceptance client that proves them (apps/aegir-cxx-smoke). ON is the
# default, so `make build` and `make run` are the full system; OFF is the lean
# freestanding build (director and the services, with the greeter and bureau as
# placeholders).
option(AEGIR_HOSTED_CXX "Build the hosted C++ runtime and its smoke client" ON)

# The GUI toolkit on top of the hosted runtime, a separate switch because the
# toolkit is its own arc (specs/cxx.md) and is still growing. It requires the
# hosted runtime. While it is OFF the greeter and bureau fall back to
# freestanding placeholders, because director refuses to boot when a manifest
# entry names a binary the initrd does not carry
# (apps/aegir-director/src/services.cc).
option(AEGIR_TOOLKIT "Build the GUI toolkit (requires AEGIR_HOSTED_CXX)" ON)
if(AEGIR_TOOLKIT AND NOT AEGIR_HOSTED_CXX)
  message(FATAL_ERROR "AEGIR_TOOLKIT needs AEGIR_HOSTED_CXX (the toolkit runs on the hosted runtime)")
endif()

find_package(seL4 REQUIRED)
find_package(elfloader-tool REQUIRED)
find_package(musllibc REQUIRED)
# util_libs is not optional: the ELF loader links libcpio from it (for the CPIO
# archive it embeds), so without this import the 'cpio' target does not exist and
# the loader cannot compile.
find_package(util_libs REQUIRED)

# Import libc++ static libraries built by the LLVM runtimes build. These are
# STATIC IMPORTED targets (cxx, cxxabi, unwind) produced by the runtime
# bootstrap (scripts/build_libcxx.sh) and installed per target.
if(AEGIR_HOSTED_CXX)
  include(libs/aegir-libcxx/imported.cmake)
  include(libs/aegir-musl/imported.cmake)
endif()

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

# The C *headers* a library needs to compile, without choosing the C library
# itself. The archive is the final target's choice, and the tree has two: seL4's
# musllibc for the freestanding path, and the vendored full musl for hosted
# targets. A library links this to reach <stdint.h> and friends; the final
# target links exactly one libc. The directory is seL4 musl's, which compiles
# against either -- both are musl -- and a hosted target's full-musl include
# directory is a plain -I, searched before this system one.
add_library(aegir-c-headers INTERFACE)
target_include_directories(
  aegir-c-headers SYSTEM INTERFACE $<TARGET_PROPERTY:muslc,INTERFACE_INCLUDE_DIRECTORIES>
)

# C++ policies. A *target's* policy is where it lives: libs/freestanding and
# apps/freestanding set the lean flags (-fno-exceptions -fno-rtti
# -fno-threadsafe-statics) at directory scope, and a hosted target links the
# hosted policy below first (for libc++'s includes and its -fexceptions).
# aegir-cxx-policy, which libraries link, is only C++17 and the warning set --
# a library does not export a policy.

if(AEGIR_HOSTED_CXX)
  # The hosted standard library, with exceptions and RTTI on
  # (specs/cxx.md's completion program, step 3): the user-code policy has to
  # match the libc++ archive's own _LIBCPP_ODR_SIGNATURE, which embeds the
  # exceptions choice (libcxx/include/__config), so both are built with them on.
  # -fasynchronous-unwind-tables undoes the environment's
  # -fno-asynchronous-unwind-tables (tools/seL4's environment_flags.cmake):
  # without .eh_frame a throw cannot walk a user frame. Thread-safe statics stay
  # off for the same single-threaded reason as the freestanding policy (threads
  # are step 4). -D_GNU_SOURCE is what makes musl's headers declare the POSIX
  # surface libc++ uses; _LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS works
  # around GCC 14's unusable __remove_pointer builtin (see
  # scripts/build_libcxx.sh). -D_LIBCPP_AEGIR is the tracked Aegir path grammar
  # (specs/cxx.md step 5): it must be defined here too, because path's grammar
  # predicates are inline in the header and have to match the library's. The policy links the full musl and libc++ because
  # choosing the hosted runtime *is* the policy's job.
  #
  # -O2 is not optional here. libc++'s containers are always-inline, and at -O0
  # GCC gives every inlined call its own stack slots and never reuses them: one
  # unordered_map insertion made a 33 KiB frame and overflowed a service's
  # stack. Optimized, the same function is a few hundred bytes. This is what
  # Linux and BSD do -- their packages and their libc++ are built -O2 -- and it
  # is why the frame problem does not exist there. GCC at -O1+ trips a
  # -Wmaybe-uninitialized false positive in libc++'s __hash_table, fixed by the
  # tracked patch third_party/patches/projects/llvm-project (see
  # scripts/build_libcxx.sh); -O0 would avoid the warning but keep the frame.
  add_library(aegir-cxx-policy-hosted INTERFACE)
  target_compile_features(aegir-cxx-policy-hosted INTERFACE cxx_std_17)
  target_compile_options(
    aegir-cxx-policy-hosted
    INTERFACE
      -O2
      -fasynchronous-unwind-tables
      -fexceptions
      -frtti
      -fno-threadsafe-statics
      -D_GNU_SOURCE
      -D_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS
      -D_LIBCPP_AEGIR
      -Wall
      -Wextra
      -Werror
  )
  target_link_libraries(aegir-cxx-policy-hosted INTERFACE cxx cxxabi musl_full)
endif()

# What a *library* links: C++17 and the warning set, and nothing about
# exceptions. A library's C++ policy is where it lives -- libs/freestanding
# applies the lean flags at directory scope, libs/hosted links the hosted
# policy -- because a library must not *export* its policy: a negative flag
# would land after a hosted consumer's -fexceptions with no later positive flag
# to undo it, and a freestanding binary can link any freestanding library
# because none of their objects carries an exception personality.
add_library(aegir-cxx-policy INTERFACE)
target_compile_features(aegir-cxx-policy INTERFACE cxx_std_17)
target_compile_options(aegir-cxx-policy INTERFACE -Wall -Wextra -Werror)

add_subdirectory(libs/freestanding)
if(AEGIR_HOSTED_CXX)
  add_subdirectory(libs/hosted)
endif()
# The hosted apps first: director packs them into its initrd, so they must be
# built before the task that reads them.
add_subdirectory(apps/hosted)
add_subdirectory(apps/freestanding)

# The command set on Sys:C (specs/dos.md). Each command binary is copied and
# stripped into ${CMAKE_BINARY_DIR}/sys-c/<name>, where scripts/make_disk.py
# packs it as Sys:C and sizes the AEGIR partition from it. Each entry is
# `target:name`: the target builds it, the name is the command the shell
# resolves -- lowercase, and the Amiga command's own name.
set(aegir_commands
  aegir-print:aegir-print
  aegir-read:aegir-read
  aegir-echo:aegir-echo
  aegir-copy:copy
  aegir-delete:delete
  aegir-makedir:makedir
  aegir-rename:rename
  aegir-list:list
  aegir-dir:dir
  aegir-type:type
  aegir-date:date
  aegir-wait:wait
  aegir-search:search
  aegir-sort:sort
  aegir-join:join
  aegir-more:more
  aegir-protect:protect
  aegir-filenote:filenote
  aegir-info:info
  aegir-assign:assign
  aegir-which:which
  aegir-version:version
)
set(aegir_commands_dir "${CMAKE_BINARY_DIR}/sys-c")
set(aegir_command_images)
foreach(pair IN LISTS aegir_commands)
  string(REPLACE ":" ";" parts "${pair}")
  list(GET parts 0 command)
  list(GET parts 1 flat_name)
  set(image "${aegir_commands_dir}/${flat_name}")
  add_custom_command(
    OUTPUT "${image}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${aegir_commands_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${command}>" "${image}"
    COMMAND "${CMAKE_STRIP}" "${image}"
    DEPENDS ${command}
    VERBATIM
  )
  list(APPEND aegir_command_images "${image}")
endforeach()
add_custom_target(aegir-sys-c ALL DEPENDS ${aegir_command_images})

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
