#
# aegir-fsbundle: the named bundle of filesystem helper images.
#
# The partition manager's filesystem services are chosen by data -- a GPT type
# GUID to a kind and a binary (specs/services.md) -- and the images themselves
# travel together in one blob, because a service's image is the one part of
# spawning a capability cannot carry. This is the format both the device
# manager (which builds it) and the partition manager (which reads it) share.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-fsbundle CXX)

add_library(aegir-fsbundle STATIC src/fsbundle.cc)

target_include_directories(aegir-fsbundle PUBLIC include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers have
# to come from a link (specs/build.md).
target_link_libraries(aegir-fsbundle PUBLIC aegir-cxx-policy sel4 aegir-c-headers)
