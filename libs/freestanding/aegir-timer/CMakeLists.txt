#
# aegir-timer-protocol: the protocol a timer source serves, and nothing else.
#
# One header, no code, the same shape as aegir-clock: the wire shape of a
# timer port -- the monotonic now and the sleep -- so the service and every
# client agree without guessing (specs/timer.md). The target carries
# `-protocol` because the service binary is aegir-timer.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-timer-protocol CXX)

add_library(aegir-timer-protocol INTERFACE)

target_include_directories(aegir-timer-protocol INTERFACE include)
target_link_libraries(aegir-timer-protocol INTERFACE aegir-cxx-policy aegir-c-headers)
