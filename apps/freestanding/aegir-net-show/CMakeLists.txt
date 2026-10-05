#
# aegir-net-show: read a Net: parameter through the VFS (specs/net.md). The
# acceptance's client of the Net: volume -- `net <adapter>/<parameter>` resolves
# and reads it with the ordinary file protocol. Staged on Sys:C as `net`.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-net-show CXX)

add_executable(aegir-net-show src/main.cc)

target_compile_options(aegir-net-show PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-net-show PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap
                         aegir-console-protocol aegir-ipc aegir-log aegir-vfs-client sel4
                         muslc sel4runtime
)
