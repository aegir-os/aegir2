#
# aegir-ilbm: the IFF ILBM decoder, the first datatype class's logic
# (specs/datatypes.md). It reads a whole image in memory and writes a
# canonical datatypes Bitmap; the class service and the host conformance both
# call it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-ilbm CXX)

add_library(aegir-ilbm STATIC src/ilbm.cc)

target_include_directories(aegir-ilbm PUBLIC include)

target_link_libraries(aegir-ilbm PUBLIC
    aegir-cxx-policy-hosted
    aegir-datatypes
)
