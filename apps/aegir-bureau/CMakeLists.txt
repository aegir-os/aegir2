#
# aegir-bureau: the session a greeter login starts, not a root task.
#
# The bureau is the Amiga screen as a shape of window (specs/console.md's
# login arc): a full-screen backdrop window, Workbench grey, painted once.
# It draws and exits -- the console owns the slice, so the window persists;
# the session's reclaim takes the process's memory back.
#
# It is packed into director's initrd (apps/aegir-director/CMakeLists.txt)
# under the flat name `aegir-bureau`, which is how auth's spawner addresses
# it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-bureau CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-bureau-app src/main.cc)

  target_compile_options(aegir-bureau-app PRIVATE -Wall -Wextra -Werror)

  # aegir-cxx-policy-hosted carries the hosted C++ flags and libc++; aegir-runtime
  # provides the pieces every binary needs (see libs/aegir-runtime). aegir-console
  # is the window protocol it speaks, aegir-mem the allocator and the window it
  # maps its slice through (the session spawn's give_vspace grant).
  target_link_libraries(
    aegir-bureau-app PRIVATE aegir-cxx-policy-hosted aegir-trinket aegir-bureau-lib aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                            aegir-console-protocol aegir-mem sel4 muslc sel4runtime
  )
else()
  # No hosted runtime yet: a placeholder keeps director's manifest validation
  # satisfied (it refuses to boot when a declared binary is absent from the
  # initrd). It draws nothing and is replaced the moment the flag is ON.
  add_executable(aegir-bureau-app src/placeholder.cc)

  target_compile_options(aegir-bureau-app PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-bureau-app PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()

# The initrd entry is named by the binary's basename (MakeCPIO has no rename
# argument), and the manifest addresses this service as `aegir-bureau`, so the
# CMake target's name is not what must land in the archive.
set_target_properties(aegir-bureau-app PROPERTIES OUTPUT_NAME aegir-bureau)
