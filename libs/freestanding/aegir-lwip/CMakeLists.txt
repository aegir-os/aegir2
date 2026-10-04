#
# aegir-lwip: the vendored lwIP, compiled for the freestanding target, and the
# port's configuration headers (lwipopts.h, arch/cc.h, arch/sys_arch.h).
#
# lwIP is third-party (third_party/lwip, BSD-3-Clause, specs/third_party.md).
# It is compiled here, not by an out-of-tree script, because it is a
# *freestanding* library: it must see muslc's headers and the freestanding
# flags, and those only exist inside the seL4 build. (The hosted vendored
# libraries -- zlib, libpng, libjpeg -- are out-of-tree scripts because their
# consumers are hosted.)
#
# The source list is lwIP's own core, IPv4, api and netif sets
# (third_party/lwip/src/Filelists.cmake), not a blind glob: ppp, 6LoWPAN,
# bridge, slip and the apps are the modules we did not take. lwIP guards each
# file with its feature option, so an option we leave off compiles to an empty
# object rather than being dropped from the list.
#
# What this target does *not* yet carry is the port's own glue: the sys_arch
# implementation (semaphores, mailboxes, the tcpip thread) that arch/sys_arch.h
# names, and the diag/assert/rand hooks arch/cc.h declares. Those are the next
# slice (specs/net.md); this target compiles lwIP's protocol code, which is
# what proves the port's headers and the pinned tree agree with the toolchain.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-lwip C CXX)

set(AEGIR_LWIP_DIR "${CMAKE_SOURCE_DIR}/third_party/lwip")
if(NOT EXISTS "${AEGIR_LWIP_DIR}/src/core/init.c")
  message(
    FATAL_ERROR
      "lwIP not found at ${AEGIR_LWIP_DIR}. Run 'make deps' to fetch the pinned "
      "tree (manifests/aegir.xml, specs/third_party.md)."
  )
endif()

file(GLOB AEGIR_LWIP_CORE "${AEGIR_LWIP_DIR}/src/core/*.c")
file(GLOB AEGIR_LWIP_IPV4 "${AEGIR_LWIP_DIR}/src/core/ipv4/*.c")

set(AEGIR_LWIP_SOURCES
    ${AEGIR_LWIP_CORE}
    ${AEGIR_LWIP_IPV4}
    # The tcpip thread (NO_SYS=0) and the generic Ethernet framing.
    ${AEGIR_LWIP_DIR}/src/api/tcpip.c
    ${AEGIR_LWIP_DIR}/src/netif/ethernet.c
    # The port's own code: the diag/assert/rand hooks arch/cc.h declares (C++,
    # because a diagnostic reaches aegir-runtime's line writer).
    ${CMAKE_CURRENT_SOURCE_DIR}/src/port.cc
)

add_library(lwip STATIC ${AEGIR_LWIP_SOURCES})

# Our headers first: lwipopts.h and arch/ are lwIP's port-interface includes,
# found by quoted include from lwip/opt.h and lwip/arch.h.
target_include_directories(lwip
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC "${AEGIR_LWIP_DIR}/src/include"
)

# <stdint.h>, <string.h> and friends come from muslc's staged headers, through
# the same interface every freestanding library uses; the archive itself is a
# library, so it never chooses a C library (specs/build.md). The port's diag
# hook reaches aegir-runtime's line writer, so its headers are borrowed here --
# only the include directory, not the runtime's warning policy, because lwIP's
# C sources are upstream's and are not built with -Werror.
target_link_libraries(lwip PUBLIC aegir-c-headers)
target_include_directories(
  lwip PRIVATE $<TARGET_PROPERTY:aegir-runtime,INTERFACE_INCLUDE_DIRECTORIES>
)
