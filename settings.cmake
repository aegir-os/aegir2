#
# Aegir's top-level build settings.
#
# init-build.sh passes this via -C (a CMake "initial cache file"), so everything
# here is a CACHE variable, and this file is also included by CMakeLists.txt —
# the same arrangement seL4's own projects use.
#
# The only decision made here is *which target*: all machine- and
# architecture-specific values live in configs/, so porting Aegir is a new file
# there rather than an edit here.
#
#   ../init-build.sh -DAEGIR_TARGET=riscv64-qemu-virt     # default
#

cmake_minimum_required(VERSION 3.16.0)

set(project_dir "${CMAKE_CURRENT_LIST_DIR}")
file(GLOB project_modules ${project_dir}/projects/*)
list(APPEND CMAKE_MODULE_PATH
     ${project_dir}/kernel
     ${project_dir}/tools/seL4/cmake-tool/helpers/
     ${project_dir}/tools/seL4/elfloader-tool/
     ${project_modules})

set(AEGIR_TARGET "riscv64-qemu-virt" CACHE STRING "Target configuration from configs/")

# Emit compile_commands.json in every build directory: clangd (and anything
# else that speaks the compilation-database format) reads it for the exact
# per-file flags, and the .clangd at the repository root points at it.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Emit compile_commands.json for clangd")
file(GLOB available_targets RELATIVE "${project_dir}/configs" "${project_dir}/configs/*.cmake")
list(TRANSFORM available_targets REPLACE "^(.+)\\.cmake$" "\\1")
set_property(CACHE AEGIR_TARGET PROPERTY STRINGS ${available_targets})

set(_aegir_config "${project_dir}/configs/${AEGIR_TARGET}.cmake")
if(NOT EXISTS "${_aegir_config}")
  message(FATAL_ERROR "Unknown AEGIR_TARGET '${AEGIR_TARGET}': no configs/${AEGIR_TARGET}.cmake")
endif()
include("${_aegir_config}")

# seL4's platform settings derive from PLATFORM and the Kernel* values, so this
# has to come after the target config.
include(application_settings)
correct_platform_strings()
find_package(seL4 REQUIRED)
sel4_configure_platform_settings()

# Debug by default: RELEASE off and VERIFICATION off selects the development
# configuration. This matters beyond optimisation level -- seL4's
# KernelVerificationBuild defaults ON, which forces KernelDebugBuild and
# KernelPrinting off, and without printing the kernel console syscalls that our
# root task uses do not exist. A `release`/`verification` build is a deliberate
# choice, so it is a switch here rather than a silent default.
set(RELEASE OFF CACHE BOOL "Performance optimised build")
set(VERIFICATION OFF CACHE BOOL "Only verification friendly kernel features")
ApplyCommonReleaseVerificationSettings(${RELEASE} ${VERIFICATION})

set(valid_platforms ${KernelPlatform_all_strings} ${correct_platform_strings_platform_aliases})
set_property(CACHE PLATFORM PROPERTY STRINGS ${valid_platforms})
if(NOT "${PLATFORM}" IN_LIST valid_platforms)
  message(FATAL_ERROR "Invalid PLATFORM '${PLATFORM}'. Valid: ${valid_platforms}")
endif()
