#
# aegir-runtime: the small pieces every Aegir userland binary needs.
#
# Not a C library (musl is that) and not a C++ standard library (we have none):
# this is where Aegir's own runtime obligations live, starting with the
# assertions that libsel4's headers emit and the console those assertions and
# our programs write to.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-runtime CXX)

add_library(aegir-runtime STATIC src/assert.cc src/debug.cc)

target_include_directories(aegir-runtime PUBLIC include)
# sel4runtime brings the kernel's generated config headers (it links
# sel4_autoconf PUBLIC), which libsel4's own headers include.
#
# The C library is deliberately *not* linked here. A library does not choose a
# libc; the final target does, and the tree now has two: the seL4 musllibc for
# the freestanding path, and the vendored full musl for hosted targets. A
# PUBLIC muslc here would put seL4's archive on every hosted link line too, and
# two libcs do not resolve cleanly. aegir-runtime needs only the C headers
# (<stdint.h>) to compile, so it borrows muslc's include directory and leaves
# the archive to its consumers -- every one of which already links its own C
# library explicitly.
target_link_libraries(aegir-runtime PUBLIC aegir-cxx-policy sel4 sel4runtime)
target_link_libraries(aegir-runtime PRIVATE aegir-c-headers)
