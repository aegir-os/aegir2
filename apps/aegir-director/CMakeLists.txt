#
# aegir-director: Aegir's root task.
#
# It creates the first services and is the only process that begins with the
# machine's authority (specs/director.md). It boots from its own initrd -- a flat
# CPIO archive with no paths, where the entry names are the service identities
# (specs/services.md) -- and that archive is packed here.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-director CXX)

# The initrd's contents, and the flat names they are addressed by. MakeCPIO
# archives each input by its basename, and cpio_get_file returns the first match,
# so two entries sharing a name would shadow silently (specs/services.md). The
# check belongs here, where the collision would be created.
set(aegir_initrd_files
    "${CMAKE_CURRENT_SOURCE_DIR}/../../manifests/services.manifest"
    "$<TARGET_FILE:aegir-hello>"
)
set(aegir_initrd_names
    "services.manifest"
    "aegir-hello"
)
set(aegir_initrd_names_check ${aegir_initrd_names})
list(REMOVE_DUPLICATES aegir_initrd_names_check)
if(NOT "${aegir_initrd_names_check}" STREQUAL "${aegir_initrd_names}")
  message(
    FATAL_ERROR "the initrd would contain two entries with the same name: ${aegir_initrd_names}"
  )
endif()

include(cpio)
MakeCPIO(director_initrd.o "${aegir_initrd_files}" DEPENDS aegir-hello)

add_executable(aegir-director src/main.cc director_initrd.o)

target_compile_options(aegir-director PRIVATE -Wall -Wextra -Werror)

# cpio is util_libs' reader for the initrd above; everything else is ours.
target_link_libraries(
  aegir-director
  PRIVATE aegir-cxx-policy aegir-runtime aegir-mem aegir-manifest sel4 muslc sel4runtime cpio
)

include(rootserver)
DeclareRootserver(aegir-director)
