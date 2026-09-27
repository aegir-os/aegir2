#
# aegir-memory: the memory service -- one pool, chunks on demand, reclaimed by
# badge (specs/memory.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-memory CXX)

add_executable(aegir-memory src/main.cc)

target_compile_options(aegir-memory PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-memory PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                       aegir-mem aegir-memory-protocol aegir-limits aegir-authdb aegir-vfs-client
                       sel4 muslc sel4runtime
)
