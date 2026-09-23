#
# aegir-datatypes: MIME-based datatype registry.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-datatypes CXX)

add_library(aegir-datatypes STATIC src/datatype.cc src/registry.cc)

target_include_directories(aegir-datatypes PUBLIC include)

target_link_libraries(aegir-datatypes PUBLIC aegir-trinket aegir-cxx-policy-hosted)