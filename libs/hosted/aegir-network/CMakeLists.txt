#
# aegir-network: musl's socket(2) family over the net stack's socket port.
#
# musl's socket functions issue Linux syscalls; aegir-heap's dispatcher answers
# them here, the way it answers the filesystem syscalls in files.cc. The one
# resource is the socket port (aegir-net-protocol), found by name through the
# bootstrap block. The seL4 side of the boundary, so it links aegir-cxx-policy
# and aegir-ipc like the heap, and no libc++.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-network CXX)

add_library(aegir-network STATIC src/network.cc)

target_include_directories(aegir-network PUBLIC include)

target_link_libraries(aegir-network PUBLIC aegir-cxx-policy aegir-ipc aegir-net-protocol
                                           aegir-runtime)
