#
# aegir-pattern: AmigaDOS wildcards (specs/pattern.md).
#
# No allocation, no libc++, no seL4: the matcher is a pure predicate over the
# caller's own strings. Freestanding, so a command, a filesystem or the toolkit
# may link it whether it is hosted or not.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-pattern CXX)

add_library(aegir-pattern STATIC src/pattern.cc)

target_include_directories(aegir-pattern PUBLIC include)

target_link_libraries(aegir-pattern PUBLIC aegir-cxx-policy aegir-c-headers)
