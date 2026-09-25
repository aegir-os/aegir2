#
# aegir-args-smoke: the ReadArgs library's acceptance client (specs/dos.md).
#
# Freestanding: aegir::args needs no libc++ and no seL4, and this smoke proves
# it by linking neither. It is packed into director's initrd and run at boot,
# printing ARGS_SMOKE_OK.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-args-smoke CXX)

add_executable(aegir-args-smoke src/main.cc)

target_compile_options(aegir-args-smoke PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-args-smoke PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap
                            aegir-args sel4 muslc sel4runtime
)
