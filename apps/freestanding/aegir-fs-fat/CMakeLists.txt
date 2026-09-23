#
# aegir-fs-fat: the FAT filesystem service -- one instance per FAT partition,
# started by the partition manager with the block device's port and a range
# grant rather than the whole device (specs/services.md). FAT16/32 are the
# interchange filesystems, not Aegir's own; FAT12 and ExFAT are refused by
# name (specs/fat.md).
#
# src/fat.* is the format (BPB, cluster chains, directory entries); src/main.cc
# is the attachment to the block port.
#
# It is packed into director's initrd under the flat name `aegir-fs-fat`, which
# is how the manifest addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-fs-fat CXX)

add_executable(aegir-fs-fat src/main.cc src/fat.cc)

target_compile_options(aegir-fs-fat PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-fs-fat PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-block
                       aegir-descriptor aegir-ipc aegir-log aegir-namespace aegir-partman aegir-volume sel4 muslc sel4runtime
)
