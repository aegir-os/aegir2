#
# aegir-env-smoke: the process environment's acceptance client.
#
# A spawned boot service that checks argv, the environment and the current
# directory (specs/environment.md), and says out loud whether it worked. It is
# packed into director's initrd under the flat name `aegir-env-smoke`; when
# AEGIR_HOSTED_CXX is OFF a freestanding placeholder keeps the manifest
# validation satisfied, as the cxx-smoke does.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-env-smoke CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-env-smoke src/main.cc src/checks.cc)

  target_compile_options(aegir-env-smoke PRIVATE -Wall -Wextra -Werror)

  # aegir-environment is the library under test; aegir-heap is the freeing heap
  # its std::string uses; aegir-mem and aegir-bootstrap are how it adopts the
  # untyped and the window the spawn kit gave it.
  target_link_libraries(
    aegir-env-smoke PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-environment aegir-heap aegir-runtime
                             aegir-bootstrap aegir-mem sel4 sel4runtime
  )
else()
  add_executable(aegir-env-smoke src/placeholder.cc)

  target_compile_options(aegir-env-smoke PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-env-smoke PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()
