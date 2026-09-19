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

add_executable(aegir-bureau-app src/main.cc)

target_compile_options(aegir-bureau-app PRIVATE -Wall -Wextra -Werror)

# aegir-cxx-policy carries the freestanding C++ flags; aegir-runtime provides
# the pieces every binary needs (see libs/aegir-runtime). aegir-console is the
# window protocol it speaks, aegir-mem the allocator and the window it maps
# its slice through (the session spawn's give_vspace grant).
target_link_libraries(
  aegir-bureau-app PRIVATE aegir-cxx-policy-hosted aegir-trinket aegir-bureau-lib aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                        aegir-console-protocol aegir-mem sel4 muslc sel4runtime
)
