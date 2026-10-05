#
# aegir-tftp: fetch a file from the DHCP-supplied gateway's TFTP server
# (specs/net.md). The socket port's datagram slice exercised end to end over the
# wire -- the "client socket path" test the spec names. It asks the stack for the
# gateway (nothing is compiled in) and reads blocks in the message envelope.
# Staged on Sys:C as the command `tftp`.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-tftp CXX)

add_executable(aegir-tftp src/main.cc)

target_compile_options(aegir-tftp PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-tftp PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap
                     aegir-console-protocol aegir-ipc aegir-log aegir-net-protocol
                     aegir-netcontrol aegir-timer-protocol sel4 muslc sel4runtime
)
