#
# aegir-log: the logger's port, as its callers see it.
#
# Header-only on purpose: a protocol is an interface, and the implementation of
# this one is a service (apps/aegir-logger).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-log CXX)

add_library(aegir-log INTERFACE)

target_include_directories(aegir-log INTERFACE include)
target_link_libraries(aegir-log INTERFACE aegir-cxx-policy aegir-c-headers)
