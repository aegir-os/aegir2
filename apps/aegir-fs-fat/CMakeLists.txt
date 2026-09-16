#
# aegir-fs-fat: the FAT filesystem service -- one instance per FAT partition,
# started by the partition manager with the block device's port and a range
# grant rather than the whole device (specs/services.md). FAT16/32/ExFAT are
# the interchange filesystems, not Aegir's own, and read-only to begin with.
#
# This is the skeleton: it says who it is and waits. The BPB read and the root
# directory listing land on top of it.
#
# It is packed into director's initrd under the flat name `aegir-fs-fat`, which
# is how the manifest addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-fs-fat CXX)

add_executable(aegir-fs-fat src/main.cc)

target_compile_options(aegir-fs-fat PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-fs-fat PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log sel4
                       muslc sel4runtime
)
