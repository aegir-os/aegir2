#
# aegir-resolver: resolving a host name -- Sys:S/hosts first, then DNS
# (specs/net.md).
#
# The resolver lives in the client, not the stack: it reads the hosts file
# itself through the VFS and reaches the stack's DNS only for names the file
# lacks. A client links this and calls aegir::resolve::lookup.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-resolver CXX)

add_library(aegir-resolver STATIC src/resolve.cc)

target_include_directories(aegir-resolver PUBLIC include)
target_link_libraries(
  aegir-resolver PUBLIC aegir-cxx-policy aegir-bootstrap aegir-ipc aegir-net-protocol
                        aegir-vfs-client aegir-c-headers sel4
)
