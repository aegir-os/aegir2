#
# aegir-datatypes: the image ABI a class and its caller share
# (specs/datatypes.md). Header-only for now; the client wrapper and the port
# protocol land with the broker service.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-datatypes CXX)

add_library(aegir-datatypes INTERFACE)

target_include_directories(aegir-datatypes INTERFACE include)

target_link_libraries(aegir-datatypes INTERFACE aegir-cxx-policy-hosted)
