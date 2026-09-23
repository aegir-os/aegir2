#
# aegir-virtio-blk: the driver for the block device.
#
# Director's survey finds the machine's devices and hands this one the transport for virtio
# device 2, which its manifest section asks for by id. It does the virtio handshake and
# reads the device's capacity; a request needs a virtqueue, which is the next thing.
#
# The register window, the handshake, and the virtqueue moved into
# libs/aegir-virtio when the second virtio driver made the seam visible; what
# stays here is the block device's own request shape (src/sector.cc) and the
# port it serves (specs/services.md).
#
# It is packed into director's initrd under the flat name `aegir-virtio-blk`, which is how
# the manifest addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-virtio-blk CXX)

add_executable(aegir-virtio-blk src/main.cc src/sector.cc)

target_compile_options(aegir-virtio-blk PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-virtio-blk PRIVATE aegir-cxx-policy aegir-cxx-policy-freestanding aegir-runtime aegir-bootstrap aegir-block aegir-ipc aegir-log aegir-virtio sel4
                            muslc sel4runtime
)
