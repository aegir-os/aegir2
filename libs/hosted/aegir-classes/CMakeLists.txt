#
# aegir-classes: BOOPSI-inspired class registry.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-classes CXX)

add_library(aegir-classes STATIC src/registry.cc)

target_include_directories(aegir-classes PUBLIC include)

target_link_libraries(aegir-classes PUBLIC aegir-trinket aegir-cxx-policy-hosted)