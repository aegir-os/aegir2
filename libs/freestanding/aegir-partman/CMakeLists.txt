#
# aegir-partman: the protocol the partition manager's own port serves.
#
# One header, no code: the port's name and the announce method. The
# partition manager includes it to serve, a filesystem it starts includes
# it to announce its volume's label (specs/vfs.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-partman CXX)

add_library(aegir-partman INTERFACE)

target_include_directories(aegir-partman INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers have
# to come from a link (specs/build.md).
target_link_libraries(aegir-partman INTERFACE aegir-cxx-policy sel4 aegir-c-headers)
