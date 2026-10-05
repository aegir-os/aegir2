#
# aegir-virtio-9p: the driver for the 9P transport.
#
# virtio device 9, bound from the registry row that names this binary. It owns
# the transport's one request queue, reads the export's name from the device's
# config space, and serves the transport port (aegir/p9transport.h) -- a shared
# window split into a request half and a reply half, one round trip per call.
# The 9P codec is aegir-9p; the filesystem that speaks it is a separate
# service, which is what keeps the transport free of any filesystem
# (specs/9p.md).
#
# It is packed into director's initrd under the flat name `aegir-virtio-9p`,
# which is how the registry addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-virtio-9p CXX)

add_executable(aegir-virtio-9p src/main.cc)

target_compile_options(aegir-virtio-9p PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-virtio-9p PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                            aegir-9p aegir-virtio sel4 muslc sel4runtime
)
