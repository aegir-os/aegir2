#
# aegir-netcontrol: the network stack's control port, and nothing else.
#
# One header, no code: the adapter listing and the parameters a `set` takes
# (aegir/netcontrol.h). The stack includes it to serve, a configurator chooses
# to call it, and neither has to guess what the other meant (specs/net.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-netcontrol CXX)

add_library(aegir-netcontrol INTERFACE)

target_include_directories(aegir-netcontrol INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers
# have to come from a link (specs/build.md).
target_link_libraries(aegir-netcontrol INTERFACE aegir-cxx-policy sel4 aegir-c-headers)
