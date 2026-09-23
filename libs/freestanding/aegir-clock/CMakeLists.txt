#
# aegir-clock: the protocol a time source serves, and nothing else.
#
# One header, no code, the same shape as aegir-entropy: the wire shape of a
# clock port -- the now method and its answer -- so the service and every
# client agree without guessing (specs/services.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-clock CXX)

add_library(aegir-clock INTERFACE)

target_include_directories(aegir-clock INTERFACE include)
target_link_libraries(aegir-clock INTERFACE aegir-cxx-policy aegir-c-headers)
