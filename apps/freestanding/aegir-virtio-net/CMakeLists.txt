#
# aegir-virtio-net: the driver for the network device.
#
# virtio device 1, bound from the registry row that names this binary. It owns
# the transport's two queues and serves the Ethernet link port
# (aegir/ethernet.h), which is the seam that keeps the stack free of any
# machine (specs/net.md). The MAC, the link state and the MTU are read from the
# device's config space, never compiled in.
#
# It is packed into director's initrd under the flat name `aegir-virtio-net`,
# which is how the registry addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-virtio-net CXX)

add_executable(aegir-virtio-net src/main.cc)

target_compile_options(aegir-virtio-net PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-virtio-net PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ethernet aegir-ipc
                           aegir-log aegir-virtio sel4 muslc sel4runtime
)
