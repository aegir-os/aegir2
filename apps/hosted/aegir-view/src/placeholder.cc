/*
 * aegir-view: freestanding placeholder.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Built when AEGIR_TOOLKIT is OFF. The Sys:C entry must still exist so the
 * command set stays whole, and a launcher asked for it gets a program that
 * says why it cannot open a window. The real viewer in src/main.cc replaces it
 * when the flag is ON.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("  view: placeholder -- the toolkit is not built\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
