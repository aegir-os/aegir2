#
# aegir-datatypes-broker: the session's datatypes broker (specs/datatypes.md).
#
# A session service auth starts from Sys:S/session.manifest. It owns
# datatypes.main and starts a class on demand -- as the session's user class,
# through the session launcher -- when a caller opens a file. Packed into
# director's initrd under the flat name `aegir-datatypes-broker`, which is how
# auth's spawner addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-datatypes-broker CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-datatypes-broker src/main.cc)

  target_compile_options(aegir-datatypes-broker PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-datatypes-broker PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-heap
                                   aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                                   aegir-namespace aegir-mem aegir-datatypes-protocol
                                   aegir-launch sel4 sel4runtime
  )
else()
  # No hosted runtime yet: a placeholder keeps director's manifest validation
  # satisfied (it refuses to boot when a declared binary is absent from the
  # initrd). It owns no port and is replaced the moment the flag is ON.
  add_executable(aegir-datatypes-broker src/placeholder.cc)

  target_compile_options(aegir-datatypes-broker PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-datatypes-broker PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc
                                   sel4runtime
  )
endif()
