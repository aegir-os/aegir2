#
# aegir-datatypes: the protocol a broker and a class serve, and the image ABI
# both sides share (specs/datatypes.md).
#
# One header, no code, the same shape as aegir-font and aegir-launch: the wire
# shape of `datatypes.main` and a class port, and the Info/Format a caller
# renders. The broker and the class include it to serve; a caller includes it
# and the hosted aegir-datatypes client to call.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-datatypes-protocol CXX)

add_library(aegir-datatypes-protocol INTERFACE)

target_include_directories(aegir-datatypes-protocol INTERFACE include)
# <stdint.h> comes from muslc, as it does for every protocol header.
target_link_libraries(aegir-datatypes-protocol INTERFACE aegir-cxx-policy aegir-c-headers)
