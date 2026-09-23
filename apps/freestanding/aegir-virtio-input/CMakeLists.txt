#
# aegir-virtio-input: the driver for the keyboard.
#
# virtio device 18, bound from the registry row that names this binary -- the
# third virtio driver, and the one with two queues, which is what the Queue
# object's per-instance state was for (specs/services.md). Its port serves
# poll and next (aegir/input.h), and `next` is the first port that can say
# "wait": the reply is held until the interrupt lands.
#
# It is packed into director's initrd under the flat name
# `aegir-virtio-input`, which is how the registry addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-virtio-input CXX)

add_executable(aegir-virtio-input src/main.cc)

target_compile_options(aegir-virtio-input PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-virtio-input PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-input aegir-ipc aegir-log aegir-virtio sel4
                             muslc sel4runtime
)
