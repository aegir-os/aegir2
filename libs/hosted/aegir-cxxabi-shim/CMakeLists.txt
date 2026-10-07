#
# aegir-cxxabi-shim: the C++ ABI symbols this tree's toolchain does not carry.
#
# Today two. __cxa_call_terminate, which GCC keeps in libsupc++ (libstdc++) and
# the riscv64 bare-metal toolchain omits, while libc++'s <string> references it.
# __cxa_thread_atexit_impl, which libc++abi's member registers a `thread_local`'s
# destructor against and musl's install here has no counterpart for. Both are
# hosted only -- they call libc++abi's and libc++'s own code -- so neither may
# reach a freestanding binary.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-cxxabi-shim CXX)

add_library(aegir-cxxabi-shim STATIC src/call_terminate.cc src/thread_atexit.cc)

target_link_libraries(aegir-cxxabi-shim PUBLIC aegir-cxx-policy-hosted)
