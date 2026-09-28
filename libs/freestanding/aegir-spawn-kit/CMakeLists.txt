#
# aegir-spawn-kit: the spawn kit as a first-class grant (specs/launch.md,
# specs/authority.md).
#
# A launcher holds a Kit -- the delegated caps and its own endpoints -- and
# builds each child's grants from it with one call, so no spawner reassembles
# the list. Used by auth (which grants the session's and the boot terminal) and
# by the terminal (which grants its commands, its shell and nested terminals).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-spawn-kit CXX)

add_library(aegir-spawn-kit STATIC src/kit.cc)

target_include_directories(aegir-spawn-kit PUBLIC include)
target_link_libraries(
  aegir-spawn-kit PUBLIC aegir-cxx-policy aegir-spawn aegir-bootstrap
                         aegir-console-protocol aegir-clock aegir-timer-protocol
                         aegir-log aegir-namespace aegir-memory-protocol
                         aegir-launch-protocol sel4
)
