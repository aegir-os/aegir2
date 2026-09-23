#
# aegir-fs-bfs: the Be File System service -- one instance per BFS partition,
# started by the partition manager with the block device's port and a range
# grant rather than the whole device (specs/services.md). It is Aegir's own
# filesystem; the format and its read half live in libs/aegir-bfs (specs/bfs.md).
#
# It is packed into director's initrd under the flat name `aegir-fs-bfs`, which
# is how the filesystem registry addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-fs-bfs CXX)

add_executable(aegir-fs-bfs src/main.cc)

target_compile_options(aegir-fs-bfs PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-fs-bfs PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-block aegir-bfs
                       aegir-descriptor aegir-clock aegir-ipc aegir-log aegir-namespace aegir-partman aegir-volume sel4 muslc sel4runtime
)
