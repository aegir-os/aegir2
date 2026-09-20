#
# aegir-bootstrap: the ABI between whoever creates a process and the process.
#
# The slots every Aegir process is given, and the block that says who it is
# (specs/director.md). Both halves are needed: director writes the block, a
# service reads it, and the auxv entry that connects them is Aegir's own tag.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-bootstrap CXX)

add_library(aegir-bootstrap STATIC src/bootstrap.cc)

target_include_directories(aegir-bootstrap PUBLIC include)
# sel4runtime carries sel4runtime/auxv.h, which is what a child reads its own
# vector through; muslc is where <stdint.h> comes from (specs/build.md).
target_link_libraries(aegir-bootstrap PUBLIC aegir-cxx-policy sel4 sel4runtime aegir-c-headers)
