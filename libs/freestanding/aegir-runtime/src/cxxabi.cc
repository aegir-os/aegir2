/*
 * The C++ ABI symbol a freestanding Aegir binary needs but its toolchain does
 * not carry.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * __cxa_pure_virtual is the Itanium C++ ABI's handler for calling a pure
 * virtual function: a vtable's slot for one points at it, so any class with a
 * pure virtual -- aegir::devtree::Tree::Visitor, say -- makes a binary that
 * refers to it. GCC keeps it in libstdc++'s libsupc++, which Aegir does not
 * link, so without this a freestanding link fails with "undefined symbol:
 * __cxa_pure_virtual". Calling it is undefined behaviour, and the only right
 * response in Aegir is a fatal report and a stop, exactly as an assertion gets
 * (assert.cc).
 */

#include <aegir/debug.h>

extern "C" void __cxa_pure_virtual()
{
    aegir::debug_write("\nAEGIR PANIC: pure virtual function called\n");
    aegir::halt();
}
