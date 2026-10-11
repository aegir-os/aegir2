#
# aegir-net: the network stack -- lwIP over every Ethernet link (specs/net.md).
# The device manager starts it, as it starts a driver: an untyped its objects
# and heap come out of, its own VSpace root, and the caller halves of
# devmgr.registry, timer.main and mem.main -- the last because its lwIP half
# keeps an allocator of its own and grows it from a chunk of the memory
# service's, not from the untyped this process was granted. It is packed into
# director's initrd under the flat name `aegir-net`, which is how the manifest
# addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-net CXX)

add_executable(aegir-net src/main.cc src/interface.cc src/socket.cc)

target_compile_options(aegir-net PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-net PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ethernet aegir-ipc
                    aegir-log aegir-mem aegir-net-protocol aegir-netcontrol aegir-registry
                    aegir-memory-protocol aegir-thread aegir-timer-protocol lwip sel4 muslc sel4runtime
)
