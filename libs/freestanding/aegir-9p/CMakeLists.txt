#
# aegir-9p: the 9P2000.L wire, and the port a transport serves it on.
#
# Two headers, no code. aegir/9p.h is the message codec -- the seven-byte
# header and the fields a request or a reply carries -- and nothing about how
# a message travels. aegir/p9transport.h is the port a *transport* serves it
# on: a shared window split into a request half and a reply half, one method
# to run a round trip. The separation is the point of the arc: the codec is
# the same for virtio-9p today and any other transport later, and the driver
# and the filesystem service each include what they need (specs/9p.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-9p CXX)

add_library(aegir-9p INTERFACE)

target_include_directories(aegir-9p INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers have
# to come from a link (specs/build.md). The transport port names its tag with the
# namespace's string wire, so that header comes along too.
target_link_libraries(aegir-9p INTERFACE aegir-cxx-policy aegir-namespace sel4 aegir-c-headers)
