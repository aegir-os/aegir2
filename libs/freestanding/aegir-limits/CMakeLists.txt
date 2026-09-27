#
# aegir-limits: resource limits, the file parsed (specs/limits.md).
#
# No allocation, no libc++, no seL4: the parse is a view into the caller's
# text, and resolve scans it. Freestanding, and host-testable as a pure value
# (scripts/check_limits.py).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-limits CXX)

add_library(aegir-limits STATIC src/limits.cc)

target_include_directories(aegir-limits PUBLIC include)

target_link_libraries(aegir-limits PUBLIC aegir-cxx-policy aegir-c-headers)
