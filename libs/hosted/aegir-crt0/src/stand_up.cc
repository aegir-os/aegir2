/*
 * aegir-crt0: the runtime stands a hosted program up, so the program does not
 * (specs/cxx.md's hosted runtime, specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A hosted program's stand-up -- adopt the kit the spawner granted this
 * process (the untyped, the VSpace root, the window) and init the heap on them
 * -- is `aegir::command::start`'s. Every command in the tree calls it in
 * `main`, which is a crt0's job done by hand, and it is the one Aegir-specific
 * call a program carries. The POSIX layer's acceptance is a program that knows
 * nothing of Aegir (specs/posix.md), so the call has to be the runtime's.
 *
 * This file is that: one constructor, in an object library the program links,
 * which stands the process up before any of the program's own constructors run.
 * The stand-up is idempotent afterwards (aegir::command::start answers true
 * when the heap is already up), so a program that still opens with the call --
 * every command in the tree -- is unaffected.
 *
 * A program that stands *itself* up, with its own allocator and kit (the
 * terminal, the shell, the launcher, the compiler), does not link this library:
 * the heap::init `ready_` guard is one heap per process, and the statics a
 * second stand-up would need are the program's own.
 */

#include <aegir/command.h>
#include <aegir/debug.h>

namespace {

/* Before any constructor a program itself can have (the default priority is
 * 65535) and after the ones the runtime needs first: aegir-heap's seed_musl
 * (200) points musl's syscalls at the dispatcher, aegir-runtime's exit bridge
 * (201) installs the process's exit, and aegir-heap's hosted exit (202)
 * registers it. 210 leaves those room and stays well below a program's, so the
 * heap is up before anything a program initialises can allocate. */
__attribute__((constructor(210))) void stand_up() noexcept
{
    if (!aegir::command::start("command")) {
        /* start() has named the reason on the debug console, and a process
         * with no heap has no runtime to return into. */
        aegir::halt();
    }
}

}  // namespace
