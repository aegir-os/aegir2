#
# aegir-block: the protocol a block device serves, and nothing else.
#
# One header, no code: the wire shape of a block port -- the method numbers,
# the identify answer's layout in the shared window, and how a read packs its
# sector and count into one word. The driver includes it to serve, clients
# include it to call, and neither has to guess what the other meant
# (specs/services.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-block CXX)

add_library(aegir-block INTERFACE)

target_include_directories(aegir-block INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers have
# to come from a link (specs/build.md).
target_link_libraries(aegir-block INTERFACE aegir-cxx-policy sel4 aegir-c-headers)
