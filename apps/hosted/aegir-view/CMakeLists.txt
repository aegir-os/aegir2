#
# aegir-view: the first kind-2 customer -- a windowed text viewer.
#
# A launcher starts it as a windowed program (specs/launch.md's kind 2), so it
# opens its own console window rather than sharing a console stream. It is a
# program in Sys:C, packed there by scripts/make_disk.py under the flat name
# `view`, which is how a `launch view` line resolves it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-view CXX)

if(AEGIR_TOOLKIT)
  add_executable(aegir-view src/main.cc)

  target_compile_options(aegir-view PRIVATE -Wall -Wextra -Werror)

  # The terminal's link shape, less the spawner: the hosted policy (libc++),
  # the toolkit, the runtime, and the console window protocol. aegir-heap is
  # the hosted runtime's file layer, which the viewer's read reaches the VFS
  # through (specs/cxx.md step 5).
  target_link_libraries(
    aegir-view PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-trinket aegir-runtime
                       aegir-bootstrap aegir-ipc aegir-log aegir-console-protocol
                       aegir-environment aegir-mem aegir-heap sel4 sel4runtime
  )
else()
  # No toolkit: a placeholder keeps the Sys:C entry present (specs/dos.md), so
  # a launcher asked for `view` gets a program that says why it cannot draw.
  add_executable(aegir-view src/placeholder.cc)

  target_compile_options(aegir-view PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-view PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()

set_target_properties(aegir-view PROPERTIES OUTPUT_NAME aegir-view)
