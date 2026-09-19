#
# aegir-libcxx: build libc++, libcxxabi, libunwind from vendored llvm-project.
#
# This builds the LLVM C++ standard library components as static libraries
# for bare-metal RISC-V with musl libc.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-libcxx C CXX ASM)

# Source is in the vendored llvm-project
set(LLVM_PROJECT_SOURCE_DIR "${CMAKE_SOURCE_DIR}/projects/llvm-project")
set(LIBCXX_SOURCE_DIR "${LLVM_PROJECT_SOURCE_DIR}/libcxx")
set(LIBCXXABI_SOURCE_DIR "${LLVM_PROJECT_SOURCE_DIR}/libcxxabi")
set(LIBUNWIND_SOURCE_DIR "${LLVM_PROJECT_SOURCE_DIR}/libunwind")

# Verify the vendored source exists
if(NOT EXISTS "${LIBCXX_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "libcxx not found at ${LIBCXX_SOURCE_DIR}. Run 'make deps' first.")
endif()

# ---------------------------------------------------------------------------
# libunwind: stack unwinding library (no dependencies)
# ---------------------------------------------------------------------------

set(LIBUNWIND_ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUNWIND_ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(LIBUNWIND_TARGET_TRIPLE "riscv64-unknown-elf" CACHE STRING "" FORCE)
set(LIBUNWIND_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory("${LIBUNWIND_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libunwind-build"
  EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# libcxxabi: low-level C++ ABI (typeinfo, exception handling)
# Depends on libunwind
# ---------------------------------------------------------------------------

set(LIBCXXABI_ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(LIBCXXABI_ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(LIBCXXABI_ENABLE_EXCEPTIONS ON CACHE BOOL "" FORCE)
set(LIBCXXABI_ENABLE_THREADS OFF CACHE BOOL "" FORCE)
set(LIBCXXABI_USE_COMPILER_RT OFF CACHE BOOL "" FORCE)
set(LIBCXXABI_USE_LLVM_UNWINDER OFF CACHE BOOL "" FORCE)
set(LIBCXXABI_LIBCXX_INCLUDES "${LIBCXX_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(LIBCXXABI_TARGET_TRIPLE "riscv64-unknown-elf" CACHE STRING "" FORCE)
set(LIBCXXABI_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory("${LIBCXXABI_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libcxxabi-build"
  EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# libc++: C++ standard library
# Depends on libcxxabi and libunwind
# ---------------------------------------------------------------------------

set(LIBCXX_ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(LIBCXX_ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(LIBCXX_ENABLE_EXCEPTIONS ON CACHE BOOL "" FORCE)
set(LIBCXX_ENABLE_RTTI ON CACHE BOOL "" FORCE)
set(LIBCXX_ENABLE_THREADS OFF CACHE BOOL "" FORCE)
set(LIBCXX_ENABLE_FILESYSTEM ON CACHE BOOL "" FORCE)
set(LIBCXX_ENABLE_LOCALIZATION ON CACHE BOOL "" FORCE)
set(LIBCXX_HAS_MUSL_LIBC ON CACHE BOOL "" FORCE)
set(LIBCXX_CXX_ABI libcxxabi CACHE STRING "" FORCE)
set(LIBCXX_CXX_ABI_INCLUDE_PATHS "${LIBCXXABI_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(LIBCXX_CXX_ABI_LIBRARY_PATH "${CMAKE_CURRENT_BINARY_DIR}/libcxxabi-build" CACHE PATH "" FORCE)
set(LIBCXX_USE_COMPILER_RT OFF CACHE BOOL "" FORCE)
set(LIBCXX_TARGET_TRIPLE "riscv64-unknown-elf" CACHE STRING "" FORCE)
set(LIBCXX_LIBUNWIND_INCLUDE_PATHS "${LIBUNWIND_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(LIBCXX_INCLUDE_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LIBCXX_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory("${LIBCXX_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libcxx-build"
  EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The subdirectories create targets: unwind, cxxabi, cxx
# Export them for downstream use
# ---------------------------------------------------------------------------

set(CXX_LIBRARIES cxx cxxabi unwind PARENT_SCOPE)
set(CXX_INCLUDE_DIRS "${LIBCXX_SOURCE_DIR}/include" PARENT_SCOPE)