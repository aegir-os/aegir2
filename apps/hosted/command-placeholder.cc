/*
 * The DOS commands' placeholder (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * With the hosted runtime off there is no runtime for a command to use, so a
 * freestanding stub keeps the Sys:C entry present -- and the AEGIR partition
 * sized as the set needs. The command would fail loudly if it were ever run,
 * and the toolkit (and so the terminal) is off in that build, so it is not.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);
    aegir::debug_write("\na DOS command: no hosted runtime in this build\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
