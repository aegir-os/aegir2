#
# aegir-launch: the protocol a launcher serves, and nothing else.
#
# One header, no code, the same shape as aegir-console: the wire shape of the
# launch.session port -- the kinds (specs/launch.md) and the methods that start
# a program or a pipeline. A launcher includes it to serve, a program that
# launches includes it and aegir-launch's client header to call.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-launch-protocol CXX)

add_library(aegir-launch-protocol INTERFACE)

target_include_directories(aegir-launch-protocol INTERFACE include)
# <stdint.h> comes from muslc, as it does for every protocol header.
target_link_libraries(aegir-launch-protocol INTERFACE aegir-cxx-policy aegir-c-headers)
