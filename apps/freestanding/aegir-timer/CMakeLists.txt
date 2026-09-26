#
# aegir-timer: the interval timer -- the RTC's alarm and interrupt served as
# timer.main (aegir/timer.h, specs/timer.md). The device manager binds the RTC
# as a platform device and starts this service with the frame and the
# interrupt, so it is a driver and not a static edge in the manifest.
#
# It is packed into director's initrd under the flat name `aegir-timer`, which
# is how the manifest and the driver registry address it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-timer CXX)

add_executable(aegir-timer src/main.cc)

target_compile_options(aegir-timer PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-timer PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap
                      aegir-timer-protocol aegir-ipc aegir-log sel4 muslc sel4runtime
)
