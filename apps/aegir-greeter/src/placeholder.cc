/*
 * aegir-greeter: freestanding placeholder.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Built when AEGIR_HOSTED_CXX is OFF, while the hosted C++ runtime the real
 * greeter needs is not yet available. It exists so director's boot-time
 * manifest validation still finds `aegir-greeter` in the initrd: a declared
 * binary that is absent stops the boot (apps/aegir-director/src/services.cc).
 * It speaks no console protocol and draws nothing. The real greeter in
 * src/main.cc replaces it as soon as the flag is ON.
 *
 * It must still signal its supervision endpoint: auth starts the greeter and
 * waits for that signal before it reports ready (apps/aegir-auth/src/main.cc's
 * start_greeter), so a placeholder that stayed silent would hang the boot
 * before the marker.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("greeter: placeholder -- hosted C++ runtime not built\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
