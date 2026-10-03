#
# aegir-datatypes: the datatypes client (specs/datatypes.md).
#
# Opens a file through a class service: it makes the class port, asks the
# session launcher to serve-launch the class under the caller's badge, and reads
# the frame a page at a time. The wire protocol and the Info/Format both sides
# share live in the freestanding aegir-datatypes-protocol; the decoded image the
# caller assembles is aegir/datatype/decoded.h.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-datatypes CXX)

add_library(aegir-datatypes STATIC src/datatypes.cc)

target_include_directories(aegir-datatypes PUBLIC include)

target_link_libraries(aegir-datatypes PUBLIC
    aegir-cxx-policy-hosted
    aegir-datatypes-protocol
    aegir-launch
    aegir-ipc
    aegir-mem
    aegir-bootstrap
    aegir-namespace
)
