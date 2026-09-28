#
# aegir-signal-smoke: the signal primitive's acceptance pair (specs/signal.md).
#
# One binary with two manifest names: `signal.smoke` serves a port with a
# Receiver and a Reply_holder, `signal.client` calls it. It is a pair because a
# held reply needs a caller blocked in a call, and one process cannot hold its
# own. The server's notification is retyped from a mem.main chunk, so it needs
# no delegated untyped -- which is Receiver::adopt's first user.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-signal-smoke CXX)

add_executable(aegir-signal-smoke src/main.cc)

target_compile_options(aegir-signal-smoke PRIVATE -Wall -Wextra -Werror)

# aegir-cxx-policy carries the freestanding C++ flags; aegir-runtime the pieces
# every binary needs; aegir-signal the primitive under test; aegir-memory-protocol
# the chunk the server's notification comes from.
target_link_libraries(
  aegir-signal-smoke PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc
                             aegir-log aegir-memory-protocol aegir-signal sel4 muslc
                             sel4runtime
)
