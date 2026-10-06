#
# aegir-break: the Amiga Break command (specs/process.md).
#
# A command of Sys:C that names a process and sets its attention flags: a client
# of process.registry (the wire is aegir/process.h) and a matcher of the NAME
# pattern (aegir-pattern, specs/pattern.md). Like every DOS command it stands
# its runtime up through aegir-command.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-break CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-break src/main.cc)
  target_compile_options(aegir-break PRIVATE -Wall -Wextra -Werror)
  # The process registry's wire (specs/process.md), by include alone: aegir-process
  # also carries the registry's table (a freestanding static library the command
  # does not need), and the client half is a header.
  target_include_directories(aegir-break PRIVATE
      ${CMAKE_SOURCE_DIR}/libs/freestanding/aegir-process/include)
  target_link_libraries(
    aegir-break PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-command
                        aegir-args aegir-heap aegir-runtime aegir-bootstrap aegir-ipc
                        aegir-pattern aegir-console-protocol aegir-mem sel4 sel4runtime
  )
else()
  add_executable(aegir-break "${AEGIR_COMMAND_PLACEHOLDER}")
  target_compile_options(aegir-break PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(
    aegir-break PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc
                        sel4runtime
  )
endif()
set_target_properties(aegir-break PROPERTIES OUTPUT_NAME aegir-break)
