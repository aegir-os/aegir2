#
# aegir-framebuffer: the protocol a display driver serves, and nothing else.
#
# One header, no code, the same shape as aegir-input and aegir-entropy: the
# wire shape of a framebuffer port -- info, flush, set_mode, and the format
# numbers. The driver includes it to serve, clients include it to call, and
# neither has to guess what the other meant (specs/services.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-framebuffer CXX)

add_library(aegir-framebuffer INTERFACE)

target_include_directories(aegir-framebuffer INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers
# have to come from a link (specs/build.md).
target_link_libraries(aegir-framebuffer INTERFACE aegir-cxx-policy aegir-c-headers)
