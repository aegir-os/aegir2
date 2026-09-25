#
# aegir-args: the Amiga's ReadArgs, as a library (specs/dos.md).
#
# No allocation, no libc++, no seL4: the parse is pointers into the caller's
# own template and argv. Freestanding, so a command may link it whether it is
# hosted or not.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-args CXX)

add_library(aegir-args STATIC src/args.cc)

target_include_directories(aegir-args PUBLIC include)

target_link_libraries(aegir-args PUBLIC aegir-cxx-policy aegir-c-headers)
