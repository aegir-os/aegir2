#
# aegir-spawn-service: the spawn machinery a service runs on (specs/launch.md).
#
# The staged allocator over a command's chunks, the reserved slot pool one owner
# per live command, and the reap that returns both -- everything a spawning
# process needs short of the grants its children get (aegir-spawn-kit). It is
# toolkit-free so the headless launcher and the terminal share it: the caller
# hands in its allocator, its scratch window and the slot range it reserved.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-spawn-service CXX)

add_library(aegir-spawn-service STATIC src/service_kit.cc src/service_launch.cc)

target_include_directories(aegir-spawn-service PUBLIC include)
# The process registry's wire (specs/process.md), by include alone: aegir-process
# carries the freestanding C++ policy and this library is hosted.
target_include_directories(aegir-spawn-service PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/freestanding/aegir-process/include)
# aegir-heap: the runtime's file layer, for the read-frame call that reads a
# program segment straight into a child's frame (specs/director.md's spawn
# path). The spawning process already runs the hosted runtime, so this is not a
# new kind of dependency.
target_link_libraries(
  aegir-spawn-service PUBLIC aegir-cxx-policy-hosted aegir-spawn aegir-spawn-kit
                            aegir-bootstrap aegir-ipc aegir-mem aegir-environment
                            aegir-console-protocol aegir-log aegir-namespace
                            aegir-memory-protocol aegir-net-protocol aegir-clock
                            aegir-timer-protocol aegir-heap sel4
)
