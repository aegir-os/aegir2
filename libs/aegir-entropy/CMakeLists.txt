#
# aegir-entropy: the protocol an entropy source serves, and nothing else.
#
# One header, no code, the same shape as aegir-block: the wire shape of an
# entropy port -- the read method, and the ceiling the port's own envelope
# sets on an answer. The driver includes it to serve, clients include it to
# call, and neither has to guess what the other meant (specs/services.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-entropy CXX)

add_library(aegir-entropy INTERFACE)

target_include_directories(aegir-entropy INTERFACE include)
# aegir-ipc for the envelope the ceiling is derived from; <stdint.h> comes
# from muslc: user code is compiled -nostdinc, so C headers have to come from
# a link (specs/build.md).
target_link_libraries(aegir-entropy INTERFACE aegir-cxx-policy aegir-ipc sel4 aegir-c-headers)
