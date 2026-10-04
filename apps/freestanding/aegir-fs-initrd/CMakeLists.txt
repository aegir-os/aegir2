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

# cpio is util_libs' reader for the archive; everything else is ours. aegir-mem
# is the window a caller's frame is mapped through for a bulk read
# (specs/vfs.md's scaling path): the maps grant, driven the same way the memory
# service drives it.
target_link_libraries(
  aegir-fs-initrd PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                          aegir-namespace aegir-volume aegir-mem sel4 muslc sel4runtime cpio
)
