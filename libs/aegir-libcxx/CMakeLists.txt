#
# aegir-libcxx: C++ standard library (libc++, libcxxabi, libunwind).
#
# These libraries are built separately via scripts/build_libcxx.sh using
# LLVM's runtimes build system (projects/llvm-project/runtimes/) and
# installed to build/libcxx-install/. The static libraries are then
# imported as STATIC IMPORTED targets via libs/aegir-libcxx/imported.cmake.
#
# This CMakeLists.txt is a placeholder to document the structure.
# The actual targets are defined in imported.cmake, which is included
# directly by the top-level CMakeLists.txt.

cmake_minimum_required(VERSION 3.16.0)

project(aegir-libcxx C CXX ASM)

# No build steps here - libraries are built externally via scripts/build_libcxx.sh
# The imported.cmake file defines the STATIC IMPORTED targets.

message(STATUS "aegir-libcxx: Using pre-built libc++ from build/libcxx-install/")