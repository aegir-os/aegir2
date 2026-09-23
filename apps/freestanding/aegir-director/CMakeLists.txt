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
# The user database, packed from its descriptor-row source into the binary
# table auth parses (specs/auth.md): the text is for people, the table is
# for the service.
set(users_db "${CMAKE_CURRENT_BINARY_DIR}/users.db")
add_custom_command(
  OUTPUT "${users_db}"
  COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/make_users.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../../manifests/users" "${users_db}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../../manifests/users"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/make_users.py"
  VERBATIM
)
add_custom_target(users_db_pack ALL DEPENDS "${users_db}")

set(aegir_initrd_files
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../manifests/services.manifest"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../manifests/drivers.registry"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../manifests/filesystems.registry"
    "${users_db}"
)
set(aegir_initrd_names
    "services.manifest"
    "drivers.registry"
    "filesystems.registry"
    "users.db"
)

# The service images, each stripped into the initrd. A service's debug sections
# are not needed at boot, and they are not free: the ELF loader relocates
# itself over a fixed span, and a rootserver grown past it stops the boot
# before the kernel prints -- which the demo client's debug sections did. The
# copies are made rather than stripped in place, so the built binaries keep
# their symbols. The name is the flat name the manifest addresses.
set(aegir_initrd_binaries
    aegir-hello
    aegir-logger
    aegir-rtc
    aegir-device-manager
    aegir-virtio-blk
    aegir-virtio-rng
    aegir-virtio-input
    aegir-virtio-gpu
    aegir-partmgr
    aegir-fs-fat
    aegir-vfs
    aegir-fs-initrd
    aegir-console
    aegir-greeter
    aegir-bureau-app
    aegir-auth
    aegir-session-smoke
    aegir-test
    aegir-cxx-smoke
    aegir-env-smoke
    aegir-fs-smoke
    aegir-gui-demo
)
set(aegir_initrd_binary_names
    "aegir-hello"
    "aegir-logger"
    "aegir-rtc"
    "aegir-device-manager"
    "aegir-virtio-blk"
    "aegir-virtio-rng"
    "aegir-virtio-input"
    "aegir-virtio-gpu"
    "aegir-partmgr"
    "aegir-fs-fat"
    "aegir-vfs"
    "aegir-fs-initrd"
    "aegir-console"
    "aegir-greeter"
    "aegir-bureau"
    "aegir-auth"
    "aegir-session-smoke"
    "aegir-test"
    "aegir-cxx-smoke"
    "aegir-env-smoke"
    "aegir-fs-smoke"
    "aegir-gui-demo"
)
set(aegir_initrd_strip_dir "${CMAKE_CURRENT_BINARY_DIR}/initrd")
list(LENGTH aegir_initrd_binaries aegir_initrd_binary_count)
math(EXPR aegir_initrd_binary_last "${aegir_initrd_binary_count} - 1")
foreach(index RANGE 0 ${aegir_initrd_binary_last})
  list(GET aegir_initrd_binaries ${index} aegir_initrd_target)
  list(GET aegir_initrd_binary_names ${index} aegir_initrd_name)
  set(aegir_initrd_image "${aegir_initrd_strip_dir}/${aegir_initrd_name}")
  add_custom_command(
    OUTPUT "${aegir_initrd_image}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${aegir_initrd_strip_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${aegir_initrd_target}>"
            "${aegir_initrd_image}"
    COMMAND "${CMAKE_STRIP}" "${aegir_initrd_image}"
    DEPENDS ${aegir_initrd_target}
    VERBATIM
  )
  list(APPEND aegir_initrd_files "${aegir_initrd_image}")
  list(APPEND aegir_initrd_names "${aegir_initrd_name}")
endforeach()

set(aegir_initrd_names_check ${aegir_initrd_names})
list(REMOVE_DUPLICATES aegir_initrd_names_check)
if(NOT "${aegir_initrd_names_check}" STREQUAL "${aegir_initrd_names}")
  message(
    FATAL_ERROR "the initrd would contain two entries with the same name: ${aegir_initrd_names}"
  )
endif()

include(cpio)
MakeCPIO(director_initrd.o "${aegir_initrd_files}" DEPENDS aegir-hello aegir-logger)

add_executable(
  aegir-director src/main.cc src/services.cc src/ports.cc src/supervisor.cc
                  director_initrd.o
)

target_compile_options(aegir-director PRIVATE -Wall -Wextra -Werror)

# cpio is util_libs' reader for the initrd above; everything else is ours.
target_link_libraries(
  aegir-director
  PRIVATE aegir-cxx-policy aegir-runtime aegir-mem aegir-bootstrap aegir-devtree aegir-ipc aegir-log
          aegir-manifest aegir-spawn sel4 muslc sel4runtime cpio
)

include(rootserver)
DeclareRootserver(aegir-director)
