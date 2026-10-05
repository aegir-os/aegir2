#
# aegir-net-volume: the Net: filesystem view (specs/net.md). A synthetic volume
# -- one directory per adapter, one file per parameter -- that reads and
# reprograms the running stack through its control port. It registers with the
# VFS and answers the volume protocol, so a client uses the ordinary file
# protocol and no new API.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-net-volume CXX)

add_executable(aegir-net-volume src/main.cc)

target_compile_options(aegir-net-volume PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-net-volume PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc
                           aegir-log aegir-namespace aegir-netcontrol aegir-volume sel4
                           muslc sel4runtime
)
