#
# aegir-virtio-blk: the driver for the block device.
#
# Director's survey finds the machine's devices and hands this one the transport for virtio
# device 2, which its manifest section asks for by id. It does the virtio handshake and
# reads the device's capacity; a request needs a virtqueue, which is the next thing.
#
# The register layout and the handshake live in src/virtio_mmio.h and are what a second
# virtio driver will need unchanged -- they move into a library when that driver exists and
# the seam is visible rather than guessed (specs/services.md).
#
# It is packed into director's initrd under the flat name `aegir-virtio-blk`, which is how
# the manifest addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-virtio-blk CXX)

add_executable(aegir-virtio-blk src/main.cc src/queue.cc)

target_compile_options(aegir-virtio-blk PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-virtio-blk PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-block aegir-ipc aegir-log sel4
                            muslc sel4runtime
)
