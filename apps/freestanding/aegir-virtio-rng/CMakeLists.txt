#
# aegir-virtio-rng: the driver for the entropy device.
#
# virtio device 4, bound from the registry row that names this binary -- the
# second virtio driver, whose arrival split the register window, the
# handshake, and the virtqueue into libs/aegir-virtio (specs/services.md).
# What stays here is the device's whole own shape: one posted buffer per
# read, answered in the port's envelope (aegir/entropy.h), so there is no
# shared window.
#
# It is packed into director's initrd under the flat name `aegir-virtio-rng`,
# which is how the registry addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-virtio-rng CXX)

add_executable(aegir-virtio-rng src/main.cc)

target_compile_options(aegir-virtio-rng PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-virtio-rng PRIVATE aegir-cxx-policy aegir-cxx-policy-freestanding aegir-runtime aegir-bootstrap aegir-entropy aegir-ipc aegir-log aegir-virtio sel4
                            muslc sel4runtime
)
