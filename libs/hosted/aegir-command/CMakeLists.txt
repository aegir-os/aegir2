#
# aegir-command: the runtime stand-up a hosted command shares (specs/dos.md).
#
# Adopt the spawner's untyped, VSpace root and window, and init the heap on
# them -- once, instead of six times. Hosted, like the commands.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-command CXX)

add_library(aegir-command STATIC src/command.cc)

target_include_directories(aegir-command PUBLIC include)

target_link_libraries(aegir-command PUBLIC
    aegir-cxx-policy-hosted
    aegir-bootstrap
    aegir-heap
    aegir-mem
    sel4
)
