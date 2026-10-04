#
# aegir-jpeg: the JPEG decoder, the third datatype class's logic
# (specs/datatypes.md). It reads a whole JPEG in memory through the vendored
# libjpeg-turbo and writes a canonical datatypes Decoded as RGB; the class
# service and the host conformance both call it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-jpeg CXX)

add_library(aegir-jpeg STATIC src/jpeg.cc)

target_include_directories(aegir-jpeg PUBLIC include)

target_link_libraries(aegir-jpeg PUBLIC
    aegir-cxx-policy-hosted
    aegir-datatypes
    jpeg
)
