/*
 * thread_atexit.cc: __cxa_thread_atexit_impl, which this tree's C++ stack calls
 * and nothing here defines (libs/hosted/aegir-cxxabi-shim).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * libc++abi's cxa_thread_atexit.cpp is built for a libc that carries glibc's
 * `__cxa_thread_atexit_impl` -- the entry a `thread_local` with a destructor
 * registers against, to be run when the calling thread exits -- and nothing in
 * this tree provides one: musl's install has no such symbol and no
 * `__cxa_thread_atexit` either (measured with nm on out/runtime/musl-install/lib
 * /libc.a). So a program that pulls that member in -- an LLVM library holding a
 * thread_local with a destructor does, which is how the linker was reached --
 * fails to link: `ld.lld: error: undefined symbol: __cxa_thread_atexit_impl`,
 * referenced by cxa_thread_atexit.cpp.o.
 *
 * Aegir's programs are single-threaded by decision (LLVM is built with
 * LLVM_ENABLE_THREADS=OFF, specs/clang-on-aegir.md:96-97) and the layer has no
 * thread-exit path to run destructors from yet, so there is no thread whose exit
 * this registration belongs to. The one thread a program has is the process, and
 * libc++abi's own atexit list already runs at its end: registering here as well
 * would either run a destructor twice or hold it for ever. So the call is
 * accepted and dropped -- the ABI's success case -- and the limitation is
 * recorded in specs/posix.md's note on threads. When Aegir grows a thread-exit
 * path, the list this needs belongs in this file.
 */

extern "C" int __cxa_thread_atexit_impl(void (*destructor)(void *), void *object,
                                        void *dso_symbol)
{
    static_cast<void>(destructor);
    static_cast<void>(object);
    static_cast<void>(dso_symbol);
    return 0;
}
