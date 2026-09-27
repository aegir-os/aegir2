#
# aegir-fs-pipe: the PIPE: handler -- named byte pipes.
#
# A volume like any other (specs/vfs.md), so the shell's `|` and a command's
# redirection reach it by path (specs/pipe.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-fs-pipe CXX)

add_executable(aegir-fs-pipe src/main.cc)

target_compile_options(aegir-fs-pipe PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-fs-pipe PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                       aegir-namespace aegir-volume sel4 muslc sel4runtime
)
