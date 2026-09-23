#
# aegir-bfs: reading a Be File System (specs/bfs.md).
#
# The format and its algorithms, free-standing: the superblock, inodes, data
# streams, and the directory B+tree. The service links it to serve, and a host
# test can link it to read an image, because nothing here uses a libc.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-bfs CXX)

add_library(aegir-bfs STATIC src/volume.cc src/allocator.cc src/inode.cc
                              src/bplustree.cc src/writer.cc src/attribute.cc
                              src/journal.cc src/query.cc)

target_include_directories(aegir-bfs PUBLIC include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers have
# to come from a link (specs/build.md).
target_link_libraries(aegir-bfs PUBLIC aegir-cxx-policy sel4 aegir-c-headers aegir-metadata)
