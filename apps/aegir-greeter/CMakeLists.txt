#
# aegir-greeter: the login window, not a root task.
#
# Auth starts one once the user database is read (specs/console.md's login
# arc): it draws the form into a console window, edits a name and a secret
# off the event channel, and asks auth.login -- a refused answer redraws, an
# accepted one is its exit, and auth reaps the windows and slice with the
# session the login earned.
#
# It is packed into director's initrd (apps/aegir-director/CMakeLists.txt)
# under the flat name `aegir-greeter`, which is how auth's spawner addresses
# it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-greeter CXX)

add_executable(aegir-greeter src/main.cc)

target_compile_options(aegir-greeter PRIVATE -Wall -Wextra -Werror)

# aegir-cxx-policy carries the freestanding C++ flags; aegir-runtime provides
# the pieces every binary needs (see libs/aegir-runtime). aegir-console is the
# window protocol it speaks, aegir-authdb the login wire and the database's
# field widths, aegir-mem the allocator and the window it maps its slice
# through (the spawn kit's give_vspace grant).
target_link_libraries(
  aegir-greeter PRIVATE aegir-cxx-policy-hosted aegir-trinket aegir-runtime aegir-bootstrap aegir-ipc
                        aegir-input aegir-console-protocol aegir-authdb aegir-namespace
                        aegir-mem sel4 muslc sel4runtime
)
