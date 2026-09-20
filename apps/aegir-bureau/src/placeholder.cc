/*
 * aegir-bureau: freestanding placeholder.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Built when AEGIR_HOSTED_CXX is OFF, while the hosted C++ runtime the real
 * bureau needs is not yet available. It exists so director's boot-time
 * manifest validation still finds `aegir-bureau` in the initrd: a declared
 * binary that is absent stops the boot (apps/aegir-director/src/services.cc).
 * It speaks no console protocol and paints nothing. The real bureau in
 * src/main.cc replaces it as soon as the flag is ON.
 *
 * The bureau is started by auth only after an accepted login, so unlike the
 * greeter it gates no boot wait; it signals its supervision endpoint anyway,
 * because every session's spawner waits on it (apps/aegir-auth/src/main.cc's
 * start_session) and a silent exit would be reported as a fault.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("bureau: placeholder -- hosted C++ runtime not built\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
