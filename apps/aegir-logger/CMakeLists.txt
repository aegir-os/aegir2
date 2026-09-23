#
# aegir-logger: the first service, and the first port.
#
# It owns log.main and renders events from whoever holds a capability to it. It
# exists early because everything else wants to say something (specs/services.md's
# boot set), and because it is the smallest thing that can prove Aegir's ports
# work: one owner, one method, one reply.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-logger CXX)

add_executable(aegir-logger src/main.cc)

target_compile_options(aegir-logger PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-logger PRIVATE aegir-cxx-policy aegir-cxx-policy-freestanding aegir-runtime aegir-bootstrap aegir-ipc aegir-log sel4 muslc
                       sel4runtime
)
