/*
 * aegir-fs-smoke: freestanding placeholder.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Built when AEGIR_HOSTED_CXX is OFF, when there is no hosted runtime for
 * aegir::filesystem to be a wrapper over. It keeps director's manifest
 * validation satisfied (a declared binary absent from the initrd stops the
 * boot) and signals its supervision endpoint so the boot thread moves on. The
 * real client in src/main.cc replaces it as soon as the flag is ON.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("fs-smoke: placeholder -- hosted C++ runtime not built\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
