#
# aegir-cxx-smoke: the hosted C++ runtime's acceptance client.
#
# A spawned boot service that stands the hosted runtime up -- the full musl's
# mallocng over aegir-heap, and libc++'s containers on top -- and says out
# loud whether it worked. It is deliberately independent of the toolkit: this
# is the runtime's own proof (specs/cxx.md), so a toolkit that is still being
# repaired cannot make the runtime look broken.
#
# It is packed into director's initrd (apps/aegir-director/CMakeLists.txt)
# under the flat name `aegir-cxx-smoke`. When AEGIR_HOSTED_CXX is OFF there is
# no runtime to exercise, so a freestanding placeholder keeps the manifest
# validation satisfied, exactly as the greeter and bureau do.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-cxx-smoke CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-cxx-smoke src/main.cc src/checks.cc)

  target_compile_options(aegir-cxx-smoke PRIVATE -Wall -Wextra -Werror)

  # The hosted policy brings libc++ and full musl; aegir-heap turns the
  # process's own memory into musl's heap; aegir-mem and aegir-bootstrap are
  # how it adopts the untyped and the window the spawn kit gave it.
  target_link_libraries(
    aegir-cxx-smoke PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-heap aegir-runtime aegir-bootstrap
                             aegir-mem sel4 sel4runtime
  )
else()
  add_executable(aegir-cxx-smoke src/placeholder.cc)

  target_compile_options(aegir-cxx-smoke PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-cxx-smoke PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()
