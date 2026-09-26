#
# aegir-heap: the freeing heap behind the C++ standard library.
#
# musl's malloc is already in the build (the vendored full musl, built with its
# default mallocng); what a process lacks is *memory*, and that is the empty
# space between musl's syscall routing (through __sysinfo) and our own vspace.
# This library fills it: a dispatcher for the memory syscalls that answers from
# pages carved out of the process's untyped and mapped into its window
# (specs/cxx.md, the "aegir-heap" bullet).
#
# It links aegir-mem (the allocator and the window), aegir-runtime (the debug
# console its failures speak on), aegir-thread (a real seL4 thread for musl's
# clone), and the full musl it serves (malloc/free are musl's, and the standard
# library's operator new/delete forward to them). src/files.cc answers the
# filesystem syscalls musl and std::filesystem make (specs/cxx.md step 5)
# through aegir-vfs-client, so that library comes too. sel4runtime and the
# policy ride along through aegir-mem, as they do for every library that
# allocates. aegir-console-protocol is the con.stream fd 0/1/2 route to
# (specs/shell.md's Phase 4).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-heap CXX)

add_library(aegir-heap STATIC src/heap.cc src/files.cc src/time.cc)

target_include_directories(aegir-heap PUBLIC include)
target_link_libraries(aegir-heap PUBLIC aegir-cxx-policy aegir-mem aegir-runtime aegir-thread aegir-vfs-client aegir-clock aegir-timer-protocol aegir-console-protocol musl_full)
