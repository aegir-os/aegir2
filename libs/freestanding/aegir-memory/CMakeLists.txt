#
# aegir-memory-protocol: the memory service's port, as its callers see it.
#
# Header-only on purpose: a protocol is an interface, and the implementation of
# this one is a service (apps/aegir-memory).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-memory-protocol CXX)

add_library(aegir-memory-protocol INTERFACE)

target_include_directories(aegir-memory-protocol INTERFACE include)
target_link_libraries(aegir-memory-protocol INTERFACE aegir-cxx-policy aegir-c-headers)
