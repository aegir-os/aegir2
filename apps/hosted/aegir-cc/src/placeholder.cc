/*
 * aegir-cc: freestanding placeholder.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Built when AEGIR_HOSTED_CXX is OFF, while the hosted C++ runtime (and the
 * LLVM libraries, which need it) are not. Director's boot-time manifest
 * validation refuses a declared binary that is absent from the initrd, so this
 * keeps `aegir-cc` present; the real driver in src/main.cc replaces it the
 * moment the flag is ON.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("cc: placeholder -- hosted C++ runtime not built\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
