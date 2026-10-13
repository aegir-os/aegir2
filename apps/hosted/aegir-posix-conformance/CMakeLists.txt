#
# aegir-posix-conformance: the POSIX memory-interface conformance suite.
#
# The tests are derived from The Open Group Base Specifications Issue 8 (IEEE
# Std 1003.1-2024), fetched by `make deps` into third_party/posix-spec/issue8.
# The standard's text is a reference; what is committed here is our own
# assertions, each naming the clause it came from (specs/posix.md).
#
# A plain POSIX program -- no Aegir call of its own -- so it must link
# aegir-crt0 first: a static archive is searched only for symbols something
# already references, and standing the runtime up is exactly what this program
# does not do itself.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-posix-conformance CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-posix-conformance src/main.cc)

  target_compile_options(aegir-posix-conformance PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-posix-conformance PRIVATE aegir-crt0 aegir-cxx-policy-hosted
                                     aegir-cxxabi-shim aegir-posix aegir-command aegir-heap
                                     aegir-runtime aegir-bootstrap aegir-mem sel4 sel4runtime
  )
else()
  add_executable(aegir-posix-conformance src/placeholder.cc)

  target_compile_options(aegir-posix-conformance PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-posix-conformance PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4
                                     muslc sel4runtime
  )
endif()
