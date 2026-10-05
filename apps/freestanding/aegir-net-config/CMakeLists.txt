#
# aegir-net-config: the stand-in for NetConfig -- it configures the stack's
# adapters through the control port (specs/net.md). net.control is the one port
# it needs; it lists the adapters and asks each for DHCP, then stops. Packed
# into director's initrd under the flat name `aegir-net-config`.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-net-config CXX)

add_executable(aegir-net-config src/main.cc)

target_compile_options(aegir-net-config PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-net-config PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc
                            aegir-log aegir-netcontrol sel4 muslc sel4runtime
)
