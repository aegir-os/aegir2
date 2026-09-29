#
# aegir-font: the protocol the font service serves, and nothing else.
#
# One header, no code, the same shape as aegir-clock and aegir-launch: the
# wire shape of the font.main port -- open a face, ask its metrics, close it
# (specs/fonts.md). The service includes it to serve; a client includes it to
# call. Nothing here needs the kernel, so it is usable from a freestanding
# consumer (the test bed) and a hosted one (the toolkit) alike.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-font-protocol CXX)

add_library(aegir-font-protocol INTERFACE)

target_include_directories(aegir-font-protocol INTERFACE include)
# <stdint.h> comes from muslc, as it does for every protocol header.
target_link_libraries(aegir-font-protocol INTERFACE aegir-cxx-policy aegir-c-headers)
