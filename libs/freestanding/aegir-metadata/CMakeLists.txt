#
# aegir-metadata: the metadata protocol (attributes and queries) a filesystem
# serves beside its volume protocol (specs/bfs.md, specs/vfs.md).
#
# One header, no code: the method numbers, the status word, the type codes and
# the well-known names. A service includes it to answer, a client to ask.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-metadata CXX)

add_library(aegir-metadata INTERFACE)

target_include_directories(aegir-metadata INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers have
# to come from a link (specs/build.md).
target_link_libraries(aegir-metadata INTERFACE aegir-cxx-policy sel4 aegir-c-headers)