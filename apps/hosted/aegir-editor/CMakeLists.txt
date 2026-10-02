#
# aegir-editor: the toolkit's multiline editor (specs/trinket/editor.md).
#
# A launcher starts it as a windowed program (specs/launch.md's kind 2), so it
# opens its own console window. Its window's content is a TabGroup with one
# TextEdit page per open file, so a file is a tab. It is a program in Sys:C,
# packed by scripts/make_disk.py under the flat name `edit`, which is how a
# `launch edit` line resolves it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-editor CXX)

if(AEGIR_TOOLKIT)
  add_executable(aegir-editor src/main.cc)

  target_compile_options(aegir-editor PRIVATE -Wall -Wextra -Werror)

  # The viewer's link shape plus the bureau protocol: the File menu is a client
  # of the menu server (specs/workbench.md), and aegir-heap is the hosted
  # runtime's file layer the editor reads and writes the VFS through
  # (specs/cxx.md step 5).
  target_link_libraries(
    aegir-editor PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-trinket aegir-runtime
                         aegir-bootstrap aegir-ipc aegir-log aegir-console-protocol
                         aegir-bureau-protocol aegir-environment aegir-mem aegir-heap
                         sel4 sel4runtime
  )
else()
  # No toolkit: a placeholder keeps the Sys:C entry present (specs/dos.md), so
  # a launcher asked for `edit` gets a program that says why it cannot draw.
  add_executable(aegir-editor src/placeholder.cc)

  target_compile_options(aegir-editor PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-editor PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()

set_target_properties(aegir-editor PROPERTIES OUTPUT_NAME aegir-editor)
