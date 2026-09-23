#
# aegir-environment: the process environment (specs/environment.md).
#
# Arguments, environment and the current directory, wrapped so a program does
# not read the startup frame or the bootstrap block itself. It is hosted C++
# (std::string), so it is gated by AEGIR_HOSTED_CXX like aegir-heap.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-environment CXX)

add_library(aegir-environment STATIC src/environment.cc)

target_include_directories(aegir-environment PUBLIC include)

target_link_libraries(aegir-environment PUBLIC
    aegir-cxx-policy-hosted
    aegir-heap
)
