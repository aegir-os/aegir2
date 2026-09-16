#
# aegir-descriptor: reading key=value row files, and nothing else.
#
# The driver registry and the partition-type table are *data* (specs/services.md):
# adding a driver is adding a row to a file, not a code change. This is the
# reader both consumers share -- one format, one parser, so the day the same
# files come from a volume instead of the initrd, only the source changes.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-descriptor CXX)

add_library(aegir-descriptor STATIC src/descriptor.cc)

target_include_directories(aegir-descriptor PUBLIC include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers have
# to come from a link (specs/build.md).
target_link_libraries(aegir-descriptor PUBLIC aegir-cxx-policy sel4 muslc)
