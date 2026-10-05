#
# aegir-tcpbulk: a bulk TCP round trip over lwIP's loopback (specs/net.md). The
# socket port's window path -- a payload past the message registers crosses in a
# frame the client carves -- exercised end to end with no wire at all. Staged on
# Sys:C as the command `tcpbulk`.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-tcpbulk CXX)

add_executable(aegir-tcpbulk src/main.cc)

target_compile_options(aegir-tcpbulk PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-tcpbulk PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-mem
                        aegir-console-protocol aegir-ipc aegir-log aegir-net-protocol
                        sel4 muslc sel4runtime
)
