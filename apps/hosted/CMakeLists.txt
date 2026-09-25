#
# The hosted apps. They link aegir-cxx-policy-hosted; the toolkit's own sit
# behind AEGIR_TOOLKIT inside their own CMakeLists.
#
# This directory is added regardless of the flags, because director's manifest
# refuses to boot when a declared binary is absent from its initrd: each app
# here builds a freestanding placeholder when its runtime is off.
#

# The DOS commands (specs/dos.md) are one hosted program each, and they share
# their shape: a main that stands the runtime up with aegir-command, parses
# its line with aegir::args and acts. This function is that shape once; each
# command's CMakeLists is one call. The placeholder keeps the Sys:C entry
# present when the hosted runtime is off (director's manifest refuses a
# declared binary that is absent).
set(AEGIR_COMMAND_PLACEHOLDER "${CMAKE_CURRENT_SOURCE_DIR}/command-placeholder.cc")
function(aegir_add_command name)
  if(AEGIR_HOSTED_CXX)
    add_executable(aegir-${name} src/main.cc)
    target_compile_options(aegir-${name} PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(
      aegir-${name} PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-command
                            aegir-args aegir-heap aegir-runtime aegir-bootstrap
                            aegir-console-protocol aegir-mem sel4 sel4runtime
    )
  else()
    add_executable(aegir-${name} "${AEGIR_COMMAND_PLACEHOLDER}")
    target_compile_options(aegir-${name} PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(
      aegir-${name} PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc
                            sel4runtime
    )
  endif()
  set_target_properties(aegir-${name} PROPERTIES OUTPUT_NAME aegir-${name})
endfunction()

add_subdirectory(aegir-cxx-smoke)
add_subdirectory(aegir-env-smoke)
add_subdirectory(aegir-fs-smoke)
add_subdirectory(aegir-print)
add_subdirectory(aegir-read)
add_subdirectory(aegir-shell)
add_subdirectory(aegir-greeter)
add_subdirectory(aegir-bureau)
add_subdirectory(aegir-terminal)
add_subdirectory(aegir-gui-demo)
add_subdirectory(aegir-copy)
add_subdirectory(aegir-delete)
add_subdirectory(aegir-makedir)
add_subdirectory(aegir-rename)
add_subdirectory(aegir-list)
add_subdirectory(aegir-type)
add_subdirectory(aegir-search)
add_subdirectory(aegir-sort)
add_subdirectory(aegir-join)
add_subdirectory(aegir-more)
