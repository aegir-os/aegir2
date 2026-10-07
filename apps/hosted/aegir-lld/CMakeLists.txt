#
# aegir-lld: lld on Aegir (specs/clang-on-aegir.md, Phase 1's deliverable).
#
# lld's own driver as a hosted program: src/main.cc is the harness, aegir-llvm is
# lld (and LLVM) as the cross-built libraries, and aegir-crt0 stands the process
# up before main -- so lld itself carries no Aegir call and no knowledge of where
# it runs. Aegir's CMake links it with the recipe every hosted program uses (the
# runtime's crt, seL4's link groups, musl, libc++ and the unwind tables), which
# is the whole reason the spec's Phase 1 needs no work inside LLVM's CMake.
#
# aegir-crt0 first, and it is an object library on purpose: a static archive is
# searched only for symbols something already references, so a stand-up
# constructor inside one would never be pulled into a program that references
# nothing from the runtime.
#
# When AEGIR_HOSTED_CXX is OFF a freestanding placeholder keeps the build whole.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-lld CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-lld src/main.cc)

  # LLVM and lld are built without RTTI, so a program including their headers
  # must be too: -frtti from the hosted policy would ask for typeinfo the
  # archives do not carry.
  target_compile_options(aegir-lld PRIVATE -Wall -Wextra -Werror -fno-rtti)

  target_link_libraries(
    aegir-lld PRIVATE aegir-crt0 aegir-cxx-policy-hosted aegir-cxxabi-shim
                      aegir-heap aegir-llvm aegir-runtime aegir-bootstrap aegir-mem
                      sel4 sel4runtime
  )
else()
  add_executable(aegir-lld src/placeholder.cc)

  target_compile_options(aegir-lld PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-lld PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc
                      sel4runtime
  )
endif()
