#
# aegir-cxxabi-shim: the C++ ABI symbols this tree's toolchain does not carry.
#
# Today one: __cxa_call_terminate, which GCC keeps in libsupc++ (libstdc++) and
# the riscv64 bare-metal toolchain omits, while libc++'s <string> references
# it. Hosted only -- the definition calls libc++abi's __cxa_begin_catch and
# std::terminate, so it must not reach a freestanding binary.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-cxxabi-shim CXX)

add_library(aegir-cxxabi-shim STATIC src/call_terminate.cc)

target_link_libraries(aegir-cxxabi-shim PUBLIC aegir-cxx-policy-hosted)
