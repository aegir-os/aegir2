#
# aegir-stack-overflow: a command that runs its stack out on purpose, for the
# pager's named overflow end (specs/pager.md).
#
# Its declared stack max is small (`-z stack-size`, the standard flag the
# spawner reads), so the pager grows the stack a step and then reports the
# overflow. The point is the report and the end status, not megabytes of
# frames, so the whole test costs one step.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-stack-overflow CXX)

add_executable(aegir-stack-overflow src/main.cc)

target_compile_options(aegir-stack-overflow PRIVATE -Wall -Wextra -Werror)

# 128 KiB: the pager maps the one step below the first pages, and the next
# fault is past the reserved base.
target_link_options(aegir-stack-overflow PRIVATE "-Wl,-z,stack-size=0x20000")

target_link_libraries(
  aegir-stack-overflow PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc
                               sel4runtime
)

set_target_properties(aegir-stack-overflow PROPERTIES OUTPUT_NAME aegir-stack-overflow)
