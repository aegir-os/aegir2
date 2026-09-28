#
# aegir-launch: the launch API a program calls (specs/launch.md).
#
# `aegir::launch` (C++) over the C runtime primitive, both filling the request
# from the caller's own environment and current directory. Hosted, like the
# programs that launch; the wire vocabulary is aegir-launch-protocol.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-launch CXX)

add_library(aegir-launch STATIC src/launch.cc)

target_include_directories(aegir-launch PUBLIC include)

target_link_libraries(aegir-launch PUBLIC
    aegir-cxx-policy-hosted
    aegir-launch-protocol
    aegir-ipc
    aegir-environment
    aegir-namespace
)
