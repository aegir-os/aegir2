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
target_link_libraries(aegir-runtime PUBLIC aegir-cxx-policy sel4 sel4runtime)
