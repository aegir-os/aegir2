#
# aegir-posix: the POSIX personality (specs/posix.md).
#
# The layer a POSIX program sees: the `/`-rooted path view, and the file,
# memory, environment and time, signal, thread and process calls such a program
# makes (specs/posix.md's arc). It is reached through the `__sysinfo` switch
# every musl syscall already lands in -- aegir-heap owns that switch and
# forwards each case here -- so the personality grows in this library and not
# in the heap: the heap keeps the memory calls it owns (the break, anonymous
# mmap, munmap), because those answer from the process's own window and have
# nothing to do with POSIX.
#
# It is hosted, so it links aegir-cxx-policy-hosted, and it reaches the
# filesystem through aegir-vfs-client: a path is resolved to a volume
# capability and the volume protocol answers the call. Aegir's own grammar
# (`Volume:rest`) is unchanged -- this library is the only place a POSIX-style
# path exists (specs/posix.md's position).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-posix CXX)

add_library(aegir-posix STATIC src/files.cc src/spawn.cc src/system.cc)

target_include_directories(aegir-posix PUBLIC include)

target_link_libraries(aegir-posix PUBLIC
    aegir-cxx-policy-hosted
    aegir-bootstrap
    aegir-launch
    aegir-mem
    aegir-vfs-client
    aegir-release
    sel4
)
