#
# aegir-spawn: Aegir's own spawn path.
#
# Reading a service out of the flat initrd, loading it into a fresh address space,
# giving it the startup frame it expects, and starting it (specs/director.md).
# Deliberately not libsel4utils (specs/userland.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-spawn CXX)

add_library(aegir-spawn STATIC src/elf.cc src/initrd.cc src/process.cc)

target_include_directories(aegir-spawn PUBLIC include)
# cpio is util_libs' reader for the initrd: the archive format is not ours to
# reimplement, but where it sits and what its flat names mean are (specs/services.md).
target_link_libraries(
  aegir-spawn PUBLIC aegir-cxx-policy aegir-bootstrap aegir-mem sel4 sel4runtime aegir-c-headers cpio
)
