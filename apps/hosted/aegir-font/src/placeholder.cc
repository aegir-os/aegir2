/*
 * aegir-font: freestanding placeholder.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Built when AEGIR_HOSTED_CXX is OFF, so director's initrd still carries the
 * name the manifest declares. It owns nothing; the real service replaces it.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("  font: placeholder -- hosted C++ runtime not built\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
