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
add_library(aegir-trinket STATIC
    src/application.cc
    src/canvas.cc
    src/widget.cc
    src/container.cc
    src/layout.cc
    src/label.cc
    src/button.cc
    src/textbox.cc
    src/panel.cc
    src/menubar.cc
    src/window.cc
    src/font.cc
    src/locale.cc
    src/theme_xen.cc
    src/worker.cc
)

target_include_directories(aegir-trinket PUBLIC include)

# Dependencies: the hosted policy (which brings libc++ and full musl), the
# console window protocol, input's event words, the framebuffer, and the log.
target_link_libraries(aegir-trinket PUBLIC
    aegir-cxx-policy-hosted
    aegir-runtime
    aegir-console-protocol
    aegir-input
    aegir-framebuffer
    aegir-log
    aegir-ipc
    sel4
)

# Copy font resources to build directory for runtime access
file(COPY resources/fonts/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/resources/fonts/)