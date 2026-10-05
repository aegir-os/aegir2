#
# aegir-net-config: NetConfig -- it reads Sys:S/network.manifest and applies it
# through the stack's control port (specs/net.md). It is a command, not a boot
# service: Startup-Sequence runs it from the boot session, so the VFS is up by
# construction when it reads the manifest. Packed into director's initrd and
# staged on Sys:C as the command `netconfig`.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-net-config CXX)

add_executable(aegir-net-config src/main.cc src/manifest.cc)

target_compile_options(aegir-net-config PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-net-config PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap
                            aegir-console-protocol aegir-ipc aegir-log aegir-netcontrol
                            aegir-resolver sel4 muslc sel4runtime
)
