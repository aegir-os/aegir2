#
# aegir-launcher: the session's launcher (specs/launch.md).
#
# It holds the session's spawn kit -- the untyped, the ASID pool, the unbadged
# console.gui, mem.main, the namespace, the log, the clock and timer -- and
# serves launch.session: the shell's commands, the bureau's Execute, and later
# a dock and the desktop icons are all its clients. auth starts it with the
# session and makes its endpoint. It is headless: it draws nothing, so it links
# no toolkit. It is packed into director's initrd under the flat name
# `aegir-launcher`, which is how auth resolves it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-launcher CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-launcher src/main.cc)

  target_compile_options(aegir-launcher PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-launcher PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-heap aegir-runtime
                           aegir-bootstrap aegir-ipc aegir-log aegir-launch aegir-memory-protocol
                           aegir-namespace aegir-spawn aegir-spawn-kit aegir-mem aegir-environment
                           sel4 sel4runtime
  )
else()
  add_executable(aegir-launcher src/placeholder.cc)

  target_compile_options(aegir-launcher PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-launcher PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()

set_target_properties(aegir-launcher PROPERTIES OUTPUT_NAME aegir-launcher)
