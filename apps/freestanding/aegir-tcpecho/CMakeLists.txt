#
# aegir-tcpecho: a TCP round trip over lwIP's loopback (specs/net.md). The socket
# port's stream slice exercised end to end -- bind, listen, connect, accept,
# write, recv -- with no wire at all. Staged on Sys:C as the command `tcpecho`.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-tcpecho CXX)

add_executable(aegir-tcpecho src/main.cc)

target_compile_options(aegir-tcpecho PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-tcpecho PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap
                        aegir-console-protocol aegir-ipc aegir-log aegir-net-protocol
                        sel4 muslc sel4runtime
)
