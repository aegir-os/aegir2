#
# aegir-vfs: the namespace, and nothing else.
#
# Owns vfs.namespace and keeps the map from a volume name to the port of the
# filesystem that serves it (specs/vfs.md). Files are not here; the map is.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-vfs CXX)

add_executable(aegir-vfs src/main.cc)

target_compile_options(aegir-vfs PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-vfs PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                    aegir-namespace sel4 muslc sel4runtime
)
