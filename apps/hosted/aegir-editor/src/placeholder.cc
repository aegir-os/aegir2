/*
 * aegir-editor: freestanding placeholder.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Built when AEGIR_TOOLKIT is OFF, so the Sys:C entry `edit` still resolves
 * (specs/dos.md): a launcher asked for `edit` gets a program that says why it
 * cannot draw. The real editor in src/main.cc replaces it when the flag is ON.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("editor: placeholder -- hosted C++ runtime not built\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
