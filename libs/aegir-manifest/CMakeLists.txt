#
# aegir-manifest: the boot manifest, parsed.
#
# Data, not policy: the format and the two rules that matter (unknown keys are
# errors; values are views into the text rather than copies) are in
# specs/services.md. Cross-entry validation belongs to director, which has the
# initrd and the whole set.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-manifest CXX)

add_library(aegir-manifest STATIC src/manifest.cc)

target_include_directories(aegir-manifest PUBLIC include)
target_link_libraries(aegir-manifest PUBLIC aegir-cxx-policy aegir-mem)
# Lean, and private: freestanding binaries link this object, a hosted one must
# not inherit -fno-exceptions from it.
target_compile_options(aegir-manifest PRIVATE -fno-exceptions -fno-rtti -fno-threadsafe-statics)
