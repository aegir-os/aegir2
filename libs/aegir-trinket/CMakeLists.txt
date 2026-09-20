#
# aegir-trinket: the Aegir GUI toolkit.
#
# Object-oriented, DPI-independent, internationalized widget toolkit
# inspired by Amiga Workbench/MUI with XEN look-and-feel.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-trinket CXX)

# Core library. locale.cc is in: full musl provides the C locale surface it
# uses. translation.cc (a fake binary format, and unused) and bidi.cc (a
# simplified UAX#9 that returns nothing, and whose only consumer is the
# BidiDirection enum in bidi.h) are the locale/BiDi arc's, not this runtime's;
# they are gated out until that arc lands (specs/cxx.md).
# The font is embedded: projects/terminus-font is a pinned vendored project
# (manifests/aegir.xml) and the VFS's file syscalls are stubbed, so the BDF is
# compiled into the library by scripts/embed_binary.py rather than read at
# runtime (specs/trinket.md).
set(AEGIR_TERMINUS_BDF "${CMAKE_SOURCE_DIR}/projects/terminus-font/ter-u12n.bdf")
set(AEGIR_TERMINUS_EMBED "${CMAKE_CURRENT_BINARY_DIR}/terminus_12_bdf.cc")
add_custom_command(
  OUTPUT "${AEGIR_TERMINUS_EMBED}"
  COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/embed_binary.py"
          "${AEGIR_TERMINUS_BDF}" "${AEGIR_TERMINUS_EMBED}" terminus_12_bdf
  DEPENDS "${AEGIR_TERMINUS_BDF}" "${CMAKE_SOURCE_DIR}/scripts/embed_binary.py"
  COMMENT "Embedding Terminus 12"
  VERBATIM
)

add_library(aegir-trinket STATIC
    src/application.cc
    src/canvas.cc
    src/widget.cc
    src/layout.cc
    src/label.cc
    src/button.cc
    src/textbox.cc
    src/panel.cc
    src/menubar.cc
    src/window.cc
    src/font.cc
    src/unicode.cc
    src/locale.cc
    src/theme_xen.cc
    src/worker.cc
    "${AEGIR_TERMINUS_EMBED}"
)

target_include_directories(aegir-trinket PUBLIC include)

# Dependencies: the hosted policy (which brings libc++ and full musl), the
# console window protocol, input's event words, the log, and the memory the
# application adopts -- aegir-bootstrap for the spawn kit's block, aegir-mem
# for the allocator and the window the slice is mapped through, and aegir-heap
# for the freeing heap the standard library allocates from.
target_link_libraries(aegir-trinket PUBLIC
    aegir-cxx-policy-hosted
    aegir-runtime
    aegir-console-protocol
    aegir-input
    aegir-log
    aegir-ipc
    aegir-bootstrap
    aegir-mem
    aegir-heap
    sel4
)

# The toolkit's font resources are vendored under projects/terminus-font and
# projects/noto-fonts; staging them into the build belongs with font loading
# (libs/aegir-trinket/src/font.cc's load_terminus), which is still a stub. This
# directory is not tracked -- git does not store an empty one -- so copy it only
# when it is present, or a fresh checkout fails to configure.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts")
  file(COPY resources/fonts/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/resources/fonts/)
endif()