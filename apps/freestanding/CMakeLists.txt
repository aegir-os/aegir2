#
# The freestanding apps: director, the services, and the drivers.
#
# The directory is the C++ policy for what is compiled here -- these are the
# binaries that link seL4's muslc and no unwinder. director is last: it packs
# the others into its initrd, so they are built before the task that reads
# them.
#

add_compile_options(-fno-exceptions -fno-rtti -fno-threadsafe-statics)

add_subdirectory(aegir-device-manager)
add_subdirectory(aegir-rtc)
add_subdirectory(aegir-timer)
add_subdirectory(aegir-virtio-blk)
add_subdirectory(aegir-virtio-rng)
add_subdirectory(aegir-virtio-input)
add_subdirectory(aegir-virtio-gpu)
add_subdirectory(aegir-partmgr)
add_subdirectory(aegir-fs-fat)
add_subdirectory(aegir-fs-bfs)
add_subdirectory(aegir-vfs)
add_subdirectory(aegir-fs-initrd)
add_subdirectory(aegir-fs-nil)
add_subdirectory(aegir-console)
add_subdirectory(aegir-test)
add_subdirectory(aegir-auth)
add_subdirectory(aegir-session-smoke)
add_subdirectory(aegir-hello)
add_subdirectory(aegir-echo)
add_subdirectory(aegir-logger)
add_subdirectory(aegir-director)
