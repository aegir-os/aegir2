#
# aegir-ping: the socket port's first client -- a raw ICMP echo (specs/net.md).
# It asks the stack's control port for the adapter's DHCP-supplied gateway and
# pings that, exercising DHCP, ARP and ICMP together. Packed into director's
# initrd under the flat name `aegir-ping`.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-ping CXX)

add_executable(aegir-ping src/main.cc)

target_compile_options(aegir-ping PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-ping PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                     aegir-net-protocol aegir-netcontrol aegir-resolver aegir-timer-protocol
                     sel4 muslc sel4runtime
)
