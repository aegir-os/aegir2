#
# aegir-mem-smoke: the memory service's client (specs/memory.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-mem-smoke CXX)

add_executable(aegir-mem-smoke src/main.cc)

target_compile_options(aegir-mem-smoke PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-mem-smoke PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc
                          aegir-log aegir-memory-protocol sel4 muslc sel4runtime
)
