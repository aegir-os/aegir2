/*
 * aegir::command: the stand-up a hosted command shares (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Every hosted command begins the same way: adopt the untyped, the VSpace
 * root and the window its spawner gave it, and init the heap on them. That is
 * a library's job, not six copies of the same fifty lines, and a command's
 * main should open with what it does rather than with how it stands up. The
 * allocator tables are static here, as they are in every hosted program: they
 * are tens of kilobytes and a process's stack is pages.
 */

#ifndef AEGIR_COMMAND_H
#define AEGIR_COMMAND_H

namespace aegir::command {

/** Stand the hosted runtime up from the kit the terminal's spawn gave this
 *  process. False -- with the reason on the debug serial -- when the untyped,
 *  the VSpace root or the window is missing, or the heap cannot claim it. A
 *  command that gets false should _Exit(127): it has no runtime to work in. */
bool start(char const *name) noexcept;

/** The heap a command runs in, in bytes. Commands are small; 8 MiB is roomy
 *  and still reclaimable with the command. */
constexpr unsigned long long kHeapBytes = 8ull << 20;

}  // namespace aegir::command

#endif  // AEGIR_COMMAND_H
