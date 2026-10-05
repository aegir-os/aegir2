#
# aegir-network: musl's socket(2) family over the net stack's socket port.
#
# musl's socket functions issue Linux syscalls; aegir-heap's dispatcher answers
# them here, the way it answers the filesystem syscalls in files.cc. The one
# resource is the socket port (aegir-net-protocol), found by name through the
# bootstrap block. The seL4 side of the boundary, so it links aegir-cxx-policy
# and aegir-ipc like the heap, and no libc++.
#
# src/resolver.cc is the other face: musl's getaddrinfo/gethostbyname are
# library functions, not syscalls, so they are replaced at link time (this
# archive precedes musl_full on a hosted link line). It reads Sys:S/hosts
# through the runtime's file layer and reaches DNS through aegir::network::resolve;
# the line grammar is aegir-resolver's hosts_line, so a hosted and a freestanding
# client match names the same way. It is the one file here that includes a musl
# header, which is why musl_full is on the line.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-network CXX)

add_library(aegir-network STATIC src/network.cc src/resolver.cc)

target_include_directories(aegir-network PUBLIC include)

target_link_libraries(aegir-network PUBLIC aegir-cxx-policy aegir-ipc aegir-net-protocol
                                           aegir-mem aegir-resolver aegir-runtime musl_full)
