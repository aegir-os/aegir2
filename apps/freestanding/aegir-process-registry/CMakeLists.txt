#
# aegir-process-registry: the live processes, named by pid (specs/process.md).
#
# A freestanding system service that owns process.registry and holds the live
# set: one Row per process, what the spawn kit registered and what a Break sets.
# It links aegir-process for the wire and the table it serves.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-process-registry CXX)

add_executable(aegir-process-registry src/main.cc)

target_compile_options(aegir-process-registry PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-process-registry PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc
                                 aegir-launch-protocol aegir-mem aegir-process sel4 muslc
                                 sel4runtime
)
