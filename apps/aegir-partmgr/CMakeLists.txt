#
# aegir-partmgr: the partition manager. The device manager starts it once block
# drivers answer, and hands it their ports; reading partition tables is a
# filesystem-agnostic job that belongs above the drivers, not inside them
# (specs/services.md).
#
# This is the skeleton: it says who it is and waits. The GPT walk and the
# filesystem launches land on top of it.
#
# It is packed into director's initrd under the flat name `aegir-partmgr`, which
# is how the manifest addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-partmgr CXX)

add_executable(aegir-partmgr src/main.cc src/gpt.cc)

target_compile_options(aegir-partmgr PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-partmgr PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-block aegir-ipc
                        aegir-log aegir-mem aegir-spawn sel4 muslc sel4runtime
)
