#
# aegir-volume: the protocol a filesystem serves on its volume port.
#
# One header, no code: the method numbers and the answer shapes. A
# filesystem includes it to serve, a client includes it to read
# (specs/vfs.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-volume CXX)

add_library(aegir-volume INTERFACE)

target_include_directories(aegir-volume INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers have
# to come from a link (specs/build.md).
target_link_libraries(aegir-volume INTERFACE aegir-cxx-policy sel4 aegir-c-headers)
