#
# aegir-script: the shell's interpreter core (specs/shell.md).
#
# The command-file parser and the frame stack the shell runs a script on. It is
# hosted C++ (std::string), so it is gated by AEGIR_HOSTED_CXX like aegir-heap;
# scripts/check_script.py compiles it against its host conformance cases.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-script CXX)

add_library(aegir-script STATIC src/interpreter.cc)

target_include_directories(aegir-script PUBLIC include)

target_link_libraries(aegir-script PUBLIC aegir-cxx-policy-hosted)
