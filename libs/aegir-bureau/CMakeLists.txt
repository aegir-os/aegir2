#
# aegir-bureau: Bureau library - menubar, window manager, desktop.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-bureau-lib CXX)

add_library(aegir-bureau-lib STATIC
    src/menubar.cc
    src/window_manager.cc
    src/desktop.cc
)

target_include_directories(aegir-bureau-lib PUBLIC include)

# Dependencies: Trinket, console, trinket's hosted policy
target_link_libraries(aegir-bureau-lib PUBLIC
    aegir-trinket
    console
    aegir-cxx-policy-hosted
)