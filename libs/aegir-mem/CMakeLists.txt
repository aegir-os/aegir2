#
# aegir-mem: Aegir's own allocator and address space.
#
# Capability slots and untyped memory (allocator), the window frames are mapped
# into so the spawner can write them (vspace), and a bump arena for the small
# structures boot needs. Deliberately ours rather than libsel4utils': see
# specs/userland.md and specs/director.md.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-mem CXX)

add_library(aegir-mem STATIC src/allocator.cc src/arena.cc src/child_vspace.cc src/vspace.cc)

target_include_directories(aegir-mem PUBLIC include)
# sel4runtime brings the kernel's generated configuration headers (it links
# sel4_autoconf PUBLIC), which is where the untyped-capacity constant and the
# object types come from. muslc supplies <stdint.h> and memcpy/memset: user code
# is compiled -nostdinc, so C headers have to come from a link (specs/build.md).
target_link_libraries(aegir-mem PUBLIC aegir-cxx-policy sel4 sel4runtime aegir-c-headers)
# Lean, and private: freestanding binaries link this object, a hosted one must
# not inherit -fno-exceptions from it.
target_compile_options(aegir-mem PRIVATE -fno-exceptions -fno-rtti -fno-threadsafe-statics)
