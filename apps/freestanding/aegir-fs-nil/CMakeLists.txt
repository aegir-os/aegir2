#
# aegir-fs-nil: the NIL: handler -- a read is EOF, a write is discarded.
#
# A volume like any other (specs/vfs.md), so redirection reaches it by path and
# the VFS needs no special case for a null device (specs/boot.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-fs-nil CXX)

add_executable(aegir-fs-nil src/main.cc)

target_compile_options(aegir-fs-nil PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-fs-nil PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                      aegir-namespace aegir-volume sel4 muslc sel4runtime
)
