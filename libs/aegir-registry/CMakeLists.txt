#
# aegir-registry: the protocol the device manager's registry port serves.
#
# One header, no code: the port's name, the method numbers, and the row the
# describe answer carries. The device manager includes it to serve, clients
# include it to ask, and neither has to guess what the other meant
# (specs/services.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-registry CXX)

add_library(aegir-registry INTERFACE)

target_include_directories(aegir-registry INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers have
# to come from a link (specs/build.md).
target_link_libraries(aegir-registry INTERFACE aegir-cxx-policy sel4 muslc)
