#
# aegir-echo: the first external command (specs/shell.md).
#
# It inherits the shell's console stream and prints its arguments to it, so it
# needs the stream's protocol and nothing else -- no window, no namespace. It
# is packed into director's initrd (apps/freestanding/aegir-director/
# CMakeLists.txt) under the flat name `aegir-echo`, which is how the shell
# resolves it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-echo CXX)

add_executable(aegir-echo src/main.cc)

target_compile_options(aegir-echo PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-echo PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc
                     aegir-console-protocol sel4 muslc sel4runtime
)
