/*
 * aegir-cxxabi-shim: the C++ ABI symbol this tree's toolchain does not carry.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * __cxa_call_terminate is the Itanium C++ ABI's "a cleanup threw while
 * unwinding" hook, and GCC normally defines it in libsupc++ (part of
 * libstdc++). The riscv64 bare-metal toolchain does not ship libstdc++, and
 * libc++abi does not define it either -- yet libc++'s <string> references it,
 * so a hosted link fails on it. The body is the ABI's own: make the exception
 * the active one, then terminate.
 */

#include <exception>

extern "C" void __cxa_begin_catch(void *exception);

extern "C" void __cxa_call_terminate(void *exception)
{
    __cxa_begin_catch(exception);
    std::terminate();
}