#
# aegir-thread: a thread inside a process.
#
# One seL4 TCB in the process's own address space, with its own stack, TLS
# block and IPC buffer. The shape a process's boot thread gets from the
# spawner, for a thread started by hand -- freestanding, because the TLS image
# is sel4runtime's (specs/userland.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-thread CXX)

add_library(aegir-thread STATIC src/thread.cc)

target_include_directories(aegir-thread PUBLIC include)
target_link_libraries(aegir-thread PUBLIC aegir-cxx-policy aegir-bootstrap aegir-mem sel4 sel4runtime)
