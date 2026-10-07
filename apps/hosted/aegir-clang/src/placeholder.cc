/*
 * aegir-clang: freestanding placeholder.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Built when AEGIR_HOSTED_CXX is OFF; it keeps the target present so the build
 * stays whole. clang itself runs when the hosted runtime is on.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("aegir-clang: placeholder -- hosted C++ runtime not built\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
