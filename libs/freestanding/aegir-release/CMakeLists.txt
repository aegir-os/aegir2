#
# aegir-release: what this system is, in one place (specs/environment.md).
#
# Header-only, and deliberately so: these are the words a program is given when
# it asks the system to identify itself, and there is nothing to link -- the POSIX
# layer's uname reads them, and anything else that says what this is can read the
# same ones. It reaches no runtime, so it serves the freestanding build as
# willingly as the hosted one.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-release CXX)

add_library(aegir-release INTERFACE)

target_include_directories(aegir-release INTERFACE include)
