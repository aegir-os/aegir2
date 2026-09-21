#
# aegir-bureau: the Workbench screen -- the desktop widget and its menus.
#
# The bureau's content is a Desktop: the backdrop, the screen title bar, and
# the always-visible menus it holds (specs/workbench.md). It is a trinket
# widget, so it links the toolkit.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-bureau-lib CXX)

# The protocol on its own, the aegir-console shape: a client includes
# aegir/bureau/menu.h to register its menus without linking the Desktop it
# draws into. menu.h models the toolkit's MenuBar, so it carries the hosted
# policy and the toolkit's headers.
add_library(aegir-bureau-protocol INTERFACE)
target_include_directories(aegir-bureau-protocol INTERFACE include)
target_link_libraries(aegir-bureau-protocol INTERFACE aegir-cxx-policy-hosted
                                                       aegir-ipc aegir-trinket
                                                       aegir-c-headers)

add_library(aegir-bureau-lib STATIC src/desktop.cc)

target_include_directories(aegir-bureau-lib PUBLIC include)

target_link_libraries(aegir-bureau-lib PUBLIC aegir-trinket aegir-cxx-policy-hosted)
