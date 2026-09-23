#
# aegir-console: the protocol the console serves, and nothing else.
#
# One header, no code, the same shape as aegir-framebuffer: the wire shape
# of the console.gui port -- attach, frame, create_window, damage,
# destroy_window, reap (specs/console.md). The console includes it to
# serve, a window's client includes it to call.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-console-protocol CXX)

add_library(aegir-console-protocol INTERFACE)

target_include_directories(aegir-console-protocol INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers
# have to come from a link (specs/build.md). aegir-ipc is the envelope, and
# aegir-input the event word's packing.
target_link_libraries(aegir-console-protocol INTERFACE aegir-cxx-policy aegir-ipc
                                                       aegir-input aegir-c-headers)
