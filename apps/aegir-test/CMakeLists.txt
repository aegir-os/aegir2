#
# aegir-test: the accumulating test bed, not a root task.
#
# Where a behaviour worth keeping gets a check that keeps it. It asks the
# questions the rest of the system answers and says out loud whether the
# answers were right, before it reports ready -- what prints after the boot
# marker is lost. Today: the VFS's half of the namespace (specs/vfs.md).
#
# It is packed into director's initrd (apps/aegir-director/CMakeLists.txt)
# under the flat name `aegir-test`, which is how it is addressed now.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-test CXX)

add_executable(aegir-test src/main.cc)

target_compile_options(aegir-test PRIVATE -Wall -Wextra -Werror)

# aegir-cxx-policy carries the freestanding C++ flags; aegir-runtime provides
# the pieces every binary needs (see libs/aegir-runtime). aegir-namespace and
# aegir-volume are the two wires it speaks: the map, and what the map points
# at (specs/vfs.md).
target_link_libraries(
  aegir-test PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                     aegir-namespace aegir-volume sel4 muslc sel4runtime
)
