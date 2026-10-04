#
# aegir-ethernet: the protocol a network link driver serves, and nothing else.
#
# One header, no code: the wire shape of the Ethernet link port -- the method
# numbers and the info answer (aegir/ethernet.h). The driver includes it to
# serve, the stack includes it to call, and neither has to guess what the other
# meant (specs/net.md, specs/services.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-ethernet CXX)

add_library(aegir-ethernet INTERFACE)

target_include_directories(aegir-ethernet INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers
# have to come from a link (specs/build.md).
target_link_libraries(aegir-ethernet INTERFACE aegir-cxx-policy sel4 aegir-c-headers)
