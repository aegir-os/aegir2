#
# aegir-terminal: the session's CON: handler and command line.
#
# The terminal owns one console window and a text grid, and runs the shell in
# it (specs/terminal.md, specs/shell.md). Auth starts it beside the bureau at
# login, with the console, the namespace and its own Home current directory.
#
# It is packed into director's initrd (apps/aegir-director/CMakeLists.txt)
# under the flat name `aegir-terminal`, which is how auth's spawner addresses
# it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-terminal CXX)

if(AEGIR_TOOLKIT)
  add_executable(aegir-terminal-app src/main.cc src/shell.cc)

  target_compile_options(aegir-terminal-app PRIVATE -Wall -Wextra -Werror)

  # The bureau's link shape: the hosted policy (libc++), the toolkit, the
  # runtime, and the console window protocol it speaks. aegir-heap is the
  # hosted runtime's file layer, which std::filesystem and the shell's Dir and
  # Type calls reach the VFS through (specs/cxx.md step 5).
  target_link_libraries(
    aegir-terminal-app PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-trinket aegir-runtime
                              aegir-bootstrap aegir-ipc aegir-log aegir-console-protocol aegir-mem
                              aegir-heap sel4 sel4runtime
  )
else()
  # No toolkit yet: a placeholder keeps director's manifest validation
  # satisfied (it refuses to boot when a declared binary is absent from the
  # initrd). It draws nothing and is replaced the moment the flag is ON.
  add_executable(aegir-terminal-app src/placeholder.cc)

  target_compile_options(aegir-terminal-app PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-terminal-app PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()

# The initrd entry is named by the binary's basename (MakeCPIO has no rename
# argument), and the manifest addresses this service as `aegir-terminal`, so
# the CMake target's name is not what must land in the archive.
set_target_properties(aegir-terminal-app PROPERTIES OUTPUT_NAME aegir-terminal)
