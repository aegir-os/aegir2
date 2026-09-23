#
# aegir-device-manager: the service that owns the machine's description of itself.
#
# Director maps the device tree into its address space and says where through the
# bootstrap block; this service reads it with libs/aegir-devtree and reports the
# buses it describes. It owns no port yet: the bus -> device -> service map and the
# drivers it will launch are what give it something to serve
# (specs/services.md).
#
# It is packed into director's initrd under the flat name `aegir-device-manager`,
# which is how the manifest addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-device-manager CXX)

add_executable(aegir-device-manager src/main.cc)

target_compile_options(aegir-device-manager PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-device-manager PRIVATE aegir-cxx-policy aegir-cxx-policy-freestanding aegir-runtime aegir-mem aegir-bootstrap aegir-block aegir-devtree
                              aegir-descriptor aegir-ipc aegir-log aegir-namespace aegir-registry aegir-spawn aegir-virtio sel4 muslc sel4runtime
)
