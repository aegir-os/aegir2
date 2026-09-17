#
# aegir-auth: the formats auth speaks -- the user database's binary table
# and the login port's protocol (specs/auth.md).
#
# One header, no code: the auth service includes it to parse and to serve,
# a caller includes it to ask.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-auth CXX)

add_library(aegir-auth INTERFACE)

target_include_directories(aegir-auth INTERFACE include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers have
# to come from a link (specs/build.md).
target_link_libraries(aegir-auth INTERFACE aegir-cxx-policy sel4 muslc)
