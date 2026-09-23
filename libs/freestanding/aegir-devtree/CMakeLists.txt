#
# aegir-devtree: reading the device tree, and nothing else.
#
# The machine's devices are written down in a device tree blob and the firmware
# gave us one, so Aegir reads it rather than probing hardware. This is the reader:
# header, structure block, strings block, and a visitor for the nodes that say what
# they are. What a device *means* is the device manager's business
# (specs/services.md).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-devtree CXX)

add_library(aegir-devtree STATIC src/devtree.cc)

target_include_directories(aegir-devtree PUBLIC include)
# <stdint.h> comes from muslc: user code is compiled -nostdinc, so C headers have
# to come from a link (specs/build.md).
target_link_libraries(aegir-devtree PUBLIC aegir-cxx-policy sel4 aegir-c-headers)
