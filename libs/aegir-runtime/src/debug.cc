/*
 * Minimum console and halt.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/debug.h>

#include <sel4/sel4.h>

namespace aegir {

void debug_write(char const *text) noexcept
{
    for (char const *cursor = text; cursor != nullptr && *cursor != '\0'; ++cursor) {
        seL4_DebugPutChar(*cursor);
    }
}

[[noreturn]] void halt() noexcept
{
    // Parking by yielding keeps the hart available to the rest of the system
    // instead of spinning with interrupts off. A thread that has nothing to do
    // is a scheduling decision, not a kernel one.
    for (;;) {
        seL4_Yield();
    }
}

}  // namespace aegir
