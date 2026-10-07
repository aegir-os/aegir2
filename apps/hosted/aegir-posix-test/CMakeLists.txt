#
# aegir-posix-test: the POSIX process surface's acceptance client (specs/posix.md).
#
# A plain C program that starts a child with `posix_spawn` and waits for it with
# `wait4` -- no Aegir-specific call -- so the runtime's POSIX face is measured
# by a program that does not know it is on Aegir.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-posix-test CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-posix-test src/main.cc)

  target_compile_options(aegir-posix-test PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-posix-test PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-command
                              aegir-heap aegir-runtime aegir-bootstrap aegir-mem sel4
                              sel4runtime
  )
else()
  add_executable(aegir-posix-test src/placeholder.cc)

  target_compile_options(aegir-posix-test PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-posix-test PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc
                              sel4runtime
  )
endif()
