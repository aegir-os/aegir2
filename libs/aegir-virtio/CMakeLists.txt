#
# aegir-virtio: the parts every virtio driver shares.
#
# The legacy-MMIO register window (mmio.h), the status handshake
# (handshake.cc), and the split virtqueue (queue.cc) -- split out of
# aegir-virtio-blk when the second driver (virtio-rng) made the seam a
# measurement rather than a guess (specs/services.md). A driver keeps only its
# device's own shapes: the config space layout and the requests it publishes.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-virtio CXX)

add_library(aegir-virtio STATIC src/handshake.cc src/queue.cc)

target_include_directories(aegir-virtio PUBLIC include)
# sel4runtime brings the kernel's generated configuration headers (it links
# sel4_autoconf PUBLIC); muslc supplies <stdint.h>: user code is compiled
# -nostdinc, so C headers have to come from a link (specs/build.md).
target_link_libraries(aegir-virtio PUBLIC aegir-cxx-policy sel4 sel4runtime aegir-c-headers)
# Lean, and private: freestanding binaries link this object, a hosted one must
# not inherit -fno-exceptions from it.
target_compile_options(aegir-virtio PRIVATE -fno-exceptions -fno-rtti -fno-threadsafe-statics)
