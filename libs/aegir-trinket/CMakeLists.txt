#
# aegir-trinket: the Aegir GUI toolkit.
#
# Object-oriented, DPI-independent, internationalized widget toolkit
# inspired by Amiga Workbench/MUI with XEN look-and-feel.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-trinket CXX)

# Core library
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
    src/translation.cc
    src/bidi.cc
    src/theme_xen.cc
    src/worker.cc
)

target_include_directories(aegir-trinket PUBLIC include)

# Dependencies: hosted C++ policy, libc++, console, input, framebuffer, log
target_link_libraries(aegir-trinket PUBLIC
    aegir-cxx-policy-hosted
    cxx
    console
    input
    framebuffer
    log
)

# Copy font resources to build directory for runtime access
file(COPY resources/fonts/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/resources/fonts/)