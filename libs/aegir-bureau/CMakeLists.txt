#
# aegir-bureau: the Workbench screen -- the desktop widget and its menus.
#
# The bureau's content is a Desktop: the backdrop, the screen title bar, and
# the always-visible menus it holds (specs/workbench.md). It is a trinket
# widget, so it links the toolkit.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-bureau-lib CXX)

add_library(aegir-bureau-lib STATIC src/desktop.cc)

target_include_directories(aegir-bureau-lib PUBLIC include)

target_link_libraries(aegir-bureau-lib PUBLIC aegir-trinket aegir-cxx-policy-hosted)
