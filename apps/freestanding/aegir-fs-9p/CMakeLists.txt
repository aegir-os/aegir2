#
# aegir-fs-9p: the 9P filesystem service.
#
# It opens the bound p9.* transport through the device manager's registry, maps
# the window it serves through, runs the 9P client (aegir/9p.h, aegir/p9client.h)
# over it, and serves the volume protocol (aegir/volume.h) by turning each call
# into a walk and a message. It registers the export's own tag as a volume, so
# the host directory a run shares is reachable by the ordinary file protocol
# (specs/9p.md).
#
# It is director-spawned like the initrd service -- a filesystem, not a driver,
# and not a partition the partition manager would start -- and packed into the
# initrd under the flat name `aegir-fs-9p`.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-fs-9p CXX)

add_executable(aegir-fs-9p src/main.cc)

target_compile_options(aegir-fs-9p PRIVATE -Wall -Wextra -Werror)

target_link_libraries(
  aegir-fs-9p PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                       aegir-9p aegir-registry aegir-volume aegir-mem sel4 muslc sel4runtime
)
