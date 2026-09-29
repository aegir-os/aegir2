#
# aegir-output: the launcher's read-only output view (specs/launch.md).
#
# A program in Sys:C, packed by scripts/make_disk.py under the flat name
# `output`, which is how the launcher resolves it when a caller with no stream
# of its own starts a command. It owns the con.stream endpoint the launcher
# made and draws what arrives; it links the launch protocol so it can release a
# command whose exit it reports.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-output CXX)

if(AEGIR_TOOLKIT)
  add_executable(aegir-output src/main.cc)

  target_compile_options(aegir-output PRIVATE -Wall -Wextra -Werror)

  # The viewer's link shape: the hosted policy (libc++), the toolkit, the
  # runtime, the console window protocol, the stream vocabulary and the launch
  # protocol its release call speaks.
  target_link_libraries(
    aegir-output PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-trinket
                         aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                         aegir-console-protocol aegir-launch-protocol aegir-mem
                         sel4 sel4runtime
  )
else()
  # No toolkit: a placeholder keeps the Sys:C entry present (specs/dos.md).
  add_executable(aegir-output src/placeholder.cc)

  target_compile_options(aegir-output PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-output PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc
                         sel4runtime
  )
endif()

set_target_properties(aegir-output PROPERTIES OUTPUT_NAME aegir-output)
