#
# aegir-signal: readiness without polling (specs/signal.md).
#
# One notification bound to the serving thread, a badge bit per waitable source,
# and two sides of it: a Receiver that owns the notification and mints a
# context's capability, a Transmitter that signals it. Reply_holder is the other
# half of a wait -- a call whose answer waits for the world saves its reply
# capability and answers later. One header, no code: the shape is the same in
# every service that needs waiting, which is the point of having it once.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-signal CXX)

add_library(aegir-signal INTERFACE)

target_include_directories(aegir-signal INTERFACE include)
# The allocator the Receiver makes its notification from, the port header for
# the badge layout (kCallMark), the kernel's own headers, and muslc's
# <stdint.h>: user code is compiled -nostdinc, so C headers come from a link
# (specs/build.md).
target_link_libraries(aegir-signal INTERFACE aegir-cxx-policy aegir-ipc aegir-mem
                                            aegir-c-headers)
