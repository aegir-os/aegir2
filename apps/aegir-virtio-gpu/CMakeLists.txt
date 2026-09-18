#
# aegir-virtio-gpu: the driver for the display.
#
# virtio device 16, bound from the registry row that names this binary -- the
# fourth virtio driver, and the first bound twice: two transports on QEMU's
# command line are two bindings of one row and one driver process per head
# (specs/services.md). Its port serves info, flush and set_mode
# (aegir/framebuffer.h); the framebuffer is its shared window, mega pages the
# device scans out of.
#
# It is packed into director's initrd under the flat name
# `aegir-virtio-gpu`, which is how the registry addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-virtio-gpu CXX)

add_executable(aegir-virtio-gpu src/main.cc src/gpu.cc)

target_compile_options(aegir-virtio-gpu PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-virtio-gpu PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-framebuffer aegir-ipc aegir-log aegir-virtio sel4
                           muslc sel4runtime
)
