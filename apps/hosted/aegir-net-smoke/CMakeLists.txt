#
# aegir-net-smoke: the musl socket shim's acceptance client (specs/net.md).
#
# A spawned boot service that uses libc's socket calls -- socket, bind, listen,
# connect, accept, send, recv -- to do a loopback echo. The shim under test is
# aegir-network, reached through aegir-heap's dispatcher; it needs the net
# stack's socket port, which its manifest `needs` grants. When AEGIR_HOSTED_CXX
# is OFF a freestanding placeholder keeps the manifest validation satisfied.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-net-smoke CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-net-smoke src/main.cc)

  target_compile_options(aegir-net-smoke PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-net-smoke PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-heap aegir-network
                             aegir-runtime aegir-bootstrap aegir-mem sel4 sel4runtime
  )
else()
  add_executable(aegir-net-smoke src/placeholder.cc)

  target_compile_options(aegir-net-smoke PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-net-smoke PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc
                             sel4runtime
  )
endif()
