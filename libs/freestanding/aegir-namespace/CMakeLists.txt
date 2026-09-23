#
# aegir-namespace: the protocol the VFS's namespace port serves.
#
# One header, no code: the port's name, the method numbers, the string wire
# shape and the row a describe answer carries. The VFS includes it to serve,
# registrants and resolvers include it to ask (specs/vfs.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-namespace CXX)

add_library(aegir-namespace INTERFACE)

target_include_directories(aegir-namespace INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers have
# to come from a link (specs/build.md).
target_link_libraries(aegir-namespace INTERFACE aegir-cxx-policy sel4 aegir-c-headers)
