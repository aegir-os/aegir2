#
# aegir-hello: the spawn smoke client, not a root task.
#
# It exists to prove the C++ path end to end -- that a C++ program compiles for
# this target, that static constructors run, and that it can talk to the kernel
# console -- and to be the cheapest thing director can start, because it needs no
# port at all. See apps/aegir-hello/src/main.cc and specs/director.md.
#
# It is packed into director's initrd (apps/aegir-director/CMakeLists.txt) under
# the flat name `aegir-hello`, which is how it is addressed now.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-hello CXX)

add_executable(aegir-hello src/main.cc)

target_compile_options(aegir-hello PRIVATE -Wall -Wextra -Werror)

# aegir-cxx-policy carries the freestanding C++ flags; aegir-runtime provides
# the pieces every binary needs (see libs/aegir-runtime).
# aegir-bootstrap is how it learns who it is (specs/director.md); aegir-ipc is
# how it talks to the port it was given, and aegir-log is that port's protocol.
target_link_libraries(
  aegir-hello PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log sel4
                     muslc sel4runtime
)
