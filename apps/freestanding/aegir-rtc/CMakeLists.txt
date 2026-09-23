#
# aegir-rtc: the time source -- the goldfish RTC mapped as a device and
# served as clock.main (aegir/clock.h). Director starts it with the RTC frame
# from the machine's inventory; it is not a device-manager driver, because a
# static port is what its consumers name (specs/services.md).
#
# It is packed into director's initrd under the flat name `aegir-rtc`, which
# is how the manifest addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-rtc CXX)

add_executable(aegir-rtc src/main.cc)

target_compile_options(aegir-rtc PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-rtc PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-clock aegir-ipc
                    aegir-log sel4 muslc sel4runtime
)
