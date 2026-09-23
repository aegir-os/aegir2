#
# aegir-fs-smoke: the filesystem library's acceptance client.
#
# A spawned boot service that speaks the VFS through both halves of the split
# (specs/cxx.md step 5): the hosted aegir::filesystem wrapper and the
# freestanding aegir::vfs transport. It is packed into director's initrd under
# the flat name `aegir-fs-smoke`; when AEGIR_HOSTED_CXX is OFF a freestanding
# placeholder keeps the manifest validation satisfied, as the other smokes do.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-fs-smoke CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-fs-smoke src/main.cc src/checks.cc)

  target_compile_options(aegir-fs-smoke PRIVATE -Wall -Wextra -Werror)

  # aegir-filesystem is the hosted wrapper under test; aegir-vfs-client is the
  # freestanding transport it and the smoke both use; aegir-heap is the hosted
  # runtime whose process this runs in.
  target_link_libraries(
    aegir-fs-smoke PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-filesystem aegir-vfs-client
                            aegir-heap aegir-runtime aegir-bootstrap aegir-mem sel4 sel4runtime
  )
else()
  add_executable(aegir-fs-smoke src/placeholder.cc)

  target_compile_options(aegir-fs-smoke PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-fs-smoke PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()
