#
# aegir-cc: the on-device compiler's first measurement (specs/clang-on-aegir.md).
#
# A spawned boot service that stands the hosted runtime up and reads a file
# through llvm::MemoryBuffer -- the way clang reads a source -- so the on-device
# compiler is measured against what Aegir actually answers. It is the first
# measurement; the stock `clang` and `lld` of Phase 3 supersede it.
#
# The build artifact is `aegir-cc`; scripts/run_target.py deploys it as `cc`
# under Sys:Development/C on the development target (specs/development.md). When
# AEGIR_HOSTED_CXX is OFF a freestanding placeholder keeps the build satisfied,
# as the other smokes do.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-cc CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-cc src/main.cc src/probe.cc)

  # LLVM and clang are built -fno-rtti, so a driver that subclasses one of their
  # types (the diagnostic consumer) must be too, or the base's typeinfo is an
  # undefined symbol at link.
  target_compile_options(aegir-cc PRIVATE -Wall -Wextra -Werror -fno-rtti)

  # aegir-cxx-policy-hosted brings libc++ and full musl; aegir-heap turns the
  # process's memory into musl's heap and answers the POSIX file calls through
  # aegir::vfs; aegir-llvm-support is the cross-built LLVM the driver links.
  target_link_libraries(
    aegir-cc PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-heap aegir-llvm
                     aegir-runtime aegir-bootstrap aegir-mem sel4 sel4runtime
  )
else()
  add_executable(aegir-cc src/placeholder.cc)

  target_compile_options(aegir-cc PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-cc PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()
