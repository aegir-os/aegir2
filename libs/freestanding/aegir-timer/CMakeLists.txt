#
# aegir-timer: the protocol a timer source serves, and nothing else.
#
# One header, no code, the same shape as aegir-clock: the wire shape of a
# timer port -- the monotonic now and the sleep -- so the service and every
# client agree without guessing (specs/timer.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-timer CXX)

add_library(aegir-timer INTERFACE)

target_include_directories(aegir-timer INTERFACE include)
target_link_libraries(aegir-timer INTERFACE aegir-cxx-policy aegir-c-headers)
