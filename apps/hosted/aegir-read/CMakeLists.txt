#
# aegir-read: a hosted command (specs/shell.md's Phase 4).
#
# It reads fd 0 -- the console stream's poll -- and echoes to fd 1, so the
# runtime's input routing and the handler's raw input queue are what it
# proves. It is packed into director's initrd under the flat name `aegir-read`,
# which is how the shell resolves it. When AEGIR_HOSTED_CXX is OFF a
# freestanding placeholder keeps the initrd entry present.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-read CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-read src/main.cc)

  target_compile_options(aegir-read PRIVATE -Wall -Wextra -Werror)

  # aegir-heap is the hosted runtime the command speaks through; aegir-console-
  # protocol is the con.stream its fd 0/1/2 route to; aegir-mem and
  # aegir-bootstrap are how it adopts the untyped and window the terminal gave
  # it.
  target_link_libraries(
    aegir-read PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-heap aegir-runtime
                       aegir-bootstrap aegir-console-protocol aegir-mem sel4 sel4runtime
  )
else()
  add_executable(aegir-read src/placeholder.cc)

  target_compile_options(aegir-read PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-read PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()

set_target_properties(aegir-read PROPERTIES OUTPUT_NAME aegir-read)