#
# aegir-datatypes: the host-side pieces of the datatypes system
# (specs/datatypes.md): the decoded image a class serves from and the client
# that talks to a class over its port. The wire protocol and the Info/Format
# both sides share live in the freestanding aegir-datatypes-protocol.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-datatypes CXX)

add_library(aegir-datatypes INTERFACE)

target_include_directories(aegir-datatypes INTERFACE include)

target_link_libraries(aegir-datatypes INTERFACE
    aegir-cxx-policy-hosted
    aegir-datatypes-protocol
)
