#
# aegir-fs-initrd: the Initrd: volume -- the boot image, served read-only.
#
# The flat archive director starts services from, served as a volume
# (specs/vfs.md): registered with the VFS by the service itself, answered
# from the archive in place through libcpio.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-fs-initrd CXX)

add_executable(aegir-fs-initrd src/main.cc)

target_compile_options(aegir-fs-initrd PRIVATE -Wall -Wextra -Werror)

# cpio is util_libs' reader for the archive; everything else is ours.
target_link_libraries(
  aegir-fs-initrd PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                          aegir-namespace aegir-volume sel4 muslc sel4runtime cpio
)
