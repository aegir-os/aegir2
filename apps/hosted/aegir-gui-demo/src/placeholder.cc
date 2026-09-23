/*
 * aegir-gui-demo: freestanding placeholder.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Built when AEGIR_TOOLKIT is OFF. It exists so director's boot-time manifest
 * validation still finds `aegir-gui-demo` in the initrd: a declared binary
 * that is absent stops the boot (apps/aegir-director/src/services.cc). It
 * draws nothing; the real demo in src/main.cc replaces it when the flag is
 * ON.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("demo: placeholder -- hosted C++ runtime not built\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
