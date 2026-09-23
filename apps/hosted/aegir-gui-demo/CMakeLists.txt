#
# aegir-gui-demo: the window manager's demonstration client, not a root task.
#
# A decorated trinket window with all three titlebar gadgets, so the window
# manager's arc is exercised end to end (specs/window-manager.md). It is
# packed into director's initrd under the flat name `aegir-gui-demo`.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-gui-demo CXX)

if(AEGIR_TOOLKIT)
  add_executable(aegir-gui-demo src/main.cc)

  target_compile_options(aegir-gui-demo PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-gui-demo PRIVATE aegir-cxx-policy-hosted aegir-trinket aegir-bureau-protocol
                           aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                           aegir-console-protocol aegir-mem sel4 sel4runtime
  )
else()
  add_executable(aegir-gui-demo src/placeholder.cc)

  target_compile_options(aegir-gui-demo PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-gui-demo PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()
