/*
 * aegir-datatypes-broker: freestanding placeholder.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Built when AEGIR_HOSTED_CXX is OFF, while the hosted C++ runtime the broker
 * needs is not yet available. Director's boot-time manifest validation refuses a
 * declared binary that is absent from the initrd
 * (apps/aegir-director/src/services.cc), so this keeps
 * `aegir-datatypes-broker` present. It owns no port and opens nothing: the
 * toolkit and the terminal are off in that build, so no client asks it to open
 * a file, and the real broker in src/main.cc replaces it the moment the flag is
 * ON.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("datatypes: placeholder -- hosted C++ runtime not built\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
