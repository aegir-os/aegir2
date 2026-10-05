#
# aegir-net-protocol: the socket port's protocol, and nothing else.
#
# One header, no code: the methods and the family/type/protocol values a client
# and the stack agree on (aegir/net.h). The stack includes it to serve, a client
# -- ping first -- includes it to call, and neither guesses at the other
# (specs/net.md). Named -protocol because the service binary is aegir-net; the
# header is aegir/net.h the way aegir/ethernet.h names aegir-ethernet.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-net-protocol CXX)

add_library(aegir-net-protocol INTERFACE)

target_include_directories(aegir-net-protocol INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers
# have to come from a link (specs/build.md).
target_link_libraries(aegir-net-protocol INTERFACE aegir-cxx-policy sel4 aegir-c-headers)
