#
# The freestanding libraries: the directory is the policy.
#
# Everything here is linkable into a freestanding binary (director and the
# services), so its objects carry -fno-exceptions and none of them exports the
# choice -- a library's policy is not a consumer's. A library that must not be
# linked into a freestanding binary belongs in libs/hosted instead.
#

# The flags are C++'s, and are scoped to C++ so a C library in this directory
# (lwip) is not handed -fno-rtti and -fno-threadsafe-statics, which GCC warns
# are not valid for C.
add_compile_options(
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
)

add_subdirectory(aegir-runtime)
add_subdirectory(aegir-devtree)
add_subdirectory(aegir-descriptor)
add_subdirectory(aegir-fsbundle)
add_subdirectory(aegir-bfs)
add_subdirectory(aegir-block)
add_subdirectory(aegir-entropy)
add_subdirectory(aegir-clock)
add_subdirectory(aegir-timer)
add_subdirectory(aegir-input)
add_subdirectory(aegir-framebuffer)
add_subdirectory(aegir-console)
add_subdirectory(aegir-launch)
add_subdirectory(aegir-font)
add_subdirectory(aegir-datatypes)
add_subdirectory(aegir-registry)
add_subdirectory(aegir-namespace)
add_subdirectory(aegir-volume)
add_subdirectory(aegir-metadata)
add_subdirectory(aegir-partman)
add_subdirectory(aegir-authdb)
add_subdirectory(aegir-mem)
add_subdirectory(aegir-bootstrap)
add_subdirectory(aegir-ipc)
add_subdirectory(aegir-signal)
add_subdirectory(aegir-vfs-client)
add_subdirectory(aegir-log)
add_subdirectory(aegir-memory)
add_subdirectory(aegir-manifest)
add_subdirectory(aegir-limits)
add_subdirectory(aegir-args)
add_subdirectory(aegir-pattern)
add_subdirectory(aegir-spawn)
add_subdirectory(aegir-spawn-kit)
add_subdirectory(aegir-thread)
add_subdirectory(aegir-virtio)
add_subdirectory(aegir-lwip)
