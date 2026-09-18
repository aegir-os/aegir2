#
# aegir-input: the protocol an input device serves, and nothing else.
#
# One header, no code, the same shape as aegir-block and aegir-entropy: the
# wire shape of an input port -- poll and next, and the event as one word.
# The driver includes it to serve, clients include it to call, and neither
# has to guess what the other meant (specs/services.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-input CXX)

add_library(aegir-input INTERFACE)

target_include_directories(aegir-input INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers
# have to come from a link (specs/build.md).
target_link_libraries(aegir-input INTERFACE aegir-cxx-policy muslc)
