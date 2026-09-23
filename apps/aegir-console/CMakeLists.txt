#
# aegir-console: the display, the pointer, and the windows.
#
# The one service that touches the display and the input devices
# (specs/console.md): it opens the gpu and the HID drivers through the
# registry, maps the gpu's pixel window into its own address space (the
# manifest's `maps` grant), and composites what its clients draw. What a
# session sees of the screen and the keyboard is what this serves.
#
# It is packed into director's initrd (apps/aegir-director/CMakeLists.txt)
# under the flat name `aegir-console`, which is how the manifest's
# [console] row addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-console CXX)

add_executable(aegir-console src/main.cc)

target_compile_options(aegir-console PRIVATE -Wall -Wextra -Werror)

# aegir-cxx-policy carries the freestanding C++ flags; aegir-runtime provides
# the pieces every binary needs (see libs/aegir-runtime). aegir-registry and
# aegir-framebuffer are the driver introductions, aegir-mem the window this
# service maps them through (specs/console.md).
target_link_libraries(
  aegir-console PRIVATE aegir-cxx-policy aegir-cxx-policy-freestanding aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                        aegir-registry aegir-framebuffer aegir-console-protocol aegir-mem sel4
                        muslc sel4runtime
)
