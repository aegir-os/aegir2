#
# aegir-png: the PNG decoder, the second datatype class's logic
# (specs/datatypes.md). It reads a whole PNG in memory through the vendored
# libpng and writes a canonical datatypes Decoded as RGBA; the class service
# and the host conformance both call it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-png CXX)

add_library(aegir-png STATIC src/png.cc)

target_include_directories(aegir-png PUBLIC include)

target_link_libraries(aegir-png PUBLIC
    aegir-cxx-policy-hosted
    aegir-datatypes
    png
)
