#
# aegir-process: the process registry's protocol and its table
# (specs/process.md).
#
# The wire (include/aegir/process.h) is one header, no code: the port's name,
# the method numbers, the attention flags and the Row -- the shape
# aegir-registry already is. The table compiled here is the value the service
# and the host test share: the live set and the authority check, which know no
# kernel (scripts/check_process.py runs them on the host).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-process CXX)

add_library(aegir-process STATIC src/process_table.cc)

target_compile_options(aegir-process PRIVATE -Wall -Wextra -Werror)

target_include_directories(aegir-process PUBLIC include)

# aegir-ipc brings the designed badge space (aegir/ipc/badge.h) the authority
# check reads; <stdint.h> comes from muslc, because user code is compiled
# -nostdinc and C headers have to come from a link (specs/build.md).
target_link_libraries(aegir-process PUBLIC aegir-cxx-policy sel4 aegir-c-headers PRIVATE aegir-ipc)
