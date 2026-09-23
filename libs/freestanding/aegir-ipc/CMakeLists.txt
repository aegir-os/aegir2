#
# aegir-ipc: ports, the way services ask each other for things.
#
# The model and the rights that make it hold are in specs/services.md; this is
# the two halves a service uses, and the reason they are two types is that a
# port has exactly one reader.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-ipc CXX)

add_library(aegir-ipc STATIC src/port.cc)

target_include_directories(aegir-ipc PUBLIC include)
target_link_libraries(aegir-ipc PUBLIC aegir-cxx-policy aegir-bootstrap sel4 sel4runtime aegir-c-headers)
