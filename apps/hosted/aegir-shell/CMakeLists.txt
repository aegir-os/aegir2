#
# aegir-shell: the Amiga command line, as its own process (specs/shell.md).
#
# It opens a cooked stream on the terminal's con.stream, reads lines, runs the
# built-ins itself, and asks the terminal to run a command -- the terminal owns
# the window and the spawn authority. It is packed into director's initrd under
# the flat name `aegir-shell`, which is how the terminal resolves it. When
# AEGIR_HOSTED_CXX is OFF a freestanding placeholder keeps the initrd entry
# present.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-shell CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-shell src/main.cc)

  target_compile_options(aegir-shell PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-shell PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-heap aegir-runtime
                        aegir-bootstrap aegir-console-protocol aegir-environment aegir-mem sel4
                        sel4runtime
  )
else()
  add_executable(aegir-shell src/placeholder.cc)

  target_compile_options(aegir-shell PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-shell PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()

set_target_properties(aegir-shell PROPERTIES OUTPUT_NAME aegir-shell)