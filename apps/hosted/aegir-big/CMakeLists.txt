#
# aegir-big: the scale acceptance's deliberately huge command (specs/memory.md).
#
# It is not packed onto Sys:C -- it is tens of megabytes, and the targets the
# ordinary tests stand on should not carry it. It is laid into the development
# target's Sys:Development/C beside the compiler (scripts/run_target.py), where
# it runs at session start, so a spawn-path cap that creeps back is caught by a
# program whose size was chosen to break the old ones.
#
# Its loaded segment is a generated blob, not a stored one: the repository holds
# a script (scripts/gen_blob.py), and the blob is built on demand. When
# AEGIR_HOSTED_CXX is OFF a placeholder keeps the build shape, as the other
# hosted programs do.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-big CXX ASM)

if(AEGIR_HOSTED_CXX)
  # The blob's size. The command's own check refuses to report OK below 32 MiB,
  # so a value here that is too small fails loudly rather than passing emptily;
  # raise it to measure a larger program (the compiler it stands beside is
  # ~84 MB).
  set(AEGIR_BIG_MIB "64" CACHE STRING "aegir-big's blob size in MiB")

  set(AEGIR_BIG_BLOB "${CMAKE_CURRENT_BINARY_DIR}/aegir-big-blob.bin")
  add_custom_command(
    OUTPUT "${AEGIR_BIG_BLOB}"
    COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/gen_blob.py"
            "${AEGIR_BIG_BLOB}" "${AEGIR_BIG_MIB}"
    DEPENDS "${CMAKE_SOURCE_DIR}/scripts/gen_blob.py"
    COMMENT "Generating aegir-big's ${AEGIR_BIG_MIB} MiB blob"
    VERBATIM
  )
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/blob.S.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/blob.S" @ONLY)

  add_executable(aegir-big src/main.cc "${CMAKE_CURRENT_BINARY_DIR}/blob.S")

  target_compile_options(aegir-big PRIVATE -Wall -Wextra -Werror)

  # The assembler includes the blob, so it must exist first.
  add_custom_target(aegir-big-blob DEPENDS "${AEGIR_BIG_BLOB}")
  add_dependencies(aegir-big aegir-big-blob)

  target_link_libraries(
    aegir-big PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-command
                      aegir-heap aegir-runtime aegir-bootstrap aegir-console-protocol
                      aegir-mem sel4 sel4runtime
  )
else()
  add_executable(aegir-big "${AEGIR_COMMAND_PLACEHOLDER}")

  target_compile_options(aegir-big PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-big PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc
                      sel4runtime
  )
endif()
