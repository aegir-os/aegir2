/*
 * aegir-stack-overflow: run the stack out on purpose (specs/pager.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The pager's overflow end is a *named* fault and a *distinct* status, and a
 * run that never overflows verifies neither. This command descends one 4 KiB
 * frame at a time until it passes the base of its reserved stack: the spawner
 * grew the stack on the faults before that, and the launcher's supervisor
 * reports this one by name (`stack-overflow pc=… sp=… base=… max=…`) and ends
 * the command with `kStackOverflowStatus` (129).
 *
 * It is a command, not a boot service: a boot service that faults is a boot
 * failure, and the acceptance wants to *observe* the end, not fail on it. Its
 * max is declared small by the link flag (`-z stack-size=0x20000`), so the
 * whole descent costs one growth step.
 */

namespace {

/* Noinline, and the bytes volatile, so the recursion is real and every frame
 * keeps its page: a compiler is otherwise free to turn a leaf recursion into a
 * loop and never touch the stack. The `depth == -1u` arm is unreachable before
 * the overflow — it is there so the function has a base case for the compiler
 * to see, not because it can be taken. */
__attribute__((noinline)) unsigned descend(unsigned depth) noexcept
{
    volatile unsigned char pad[4096];
    pad[0] = static_cast<unsigned char>(depth);
    pad[sizeof(pad) - 1] = static_cast<unsigned char>(pad[0] + 1);
    if (depth == 0xffffffffu) {
        return pad[0];
    }
    return static_cast<unsigned>(pad[0]) + descend(depth + 1);
}

}  // namespace

int main()
{
    return static_cast<int>(descend(0) & 0x7f);
}
