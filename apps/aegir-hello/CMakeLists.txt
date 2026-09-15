#
# aegir-hello: the root task.
#
# First Aegir program: it exists to prove the C++ path end to end -- that a C++
# root task compiles for this target, that static constructors run, and that it
# can talk to the kernel console. See apps/aegir-hello/src/main.cc.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-hello CXX)

add_executable(aegir-hello src/main.cc)

target_compile_options(aegir-hello PRIVATE -Wall -Wextra -Werror)

# aegir-cxx-policy carries the freestanding C++ flags; aegir-runtime provides
# the pieces every binary needs (see libs/aegir-runtime).
target_link_libraries(aegir-hello PRIVATE aegir-cxx-policy aegir-runtime sel4 muslc sel4runtime)

include(rootserver)
DeclareRootserver(aegir-hello)
