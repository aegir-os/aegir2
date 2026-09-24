/*
 * aegir-terminal: freestanding placeholder.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Built when AEGIR_TOOLKIT is OFF, while the toolkit the real terminal needs
 * is not available. It exists so director's boot-time manifest validation
 * still finds `aegir-terminal` in the initrd: a declared binary that is
 * absent stops the boot (apps/aegir-director/src/services.cc). It draws
 * nothing. The real terminal in src/main.cc replaces it as soon as the flag
 * is ON.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("terminal: placeholder -- toolkit not built\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
