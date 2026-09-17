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

void debug_write(char const *text, uint32_t length) noexcept
{
    for (uint32_t i = 0; i < length; ++i) {
        seL4_DebugPutChar(text[i]);
    }
}

void debug_write_unsigned(uint64_t value) noexcept
{
    char digits[20];
    int length = 0;
    do {
        digits[length++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && length < static_cast<int>(sizeof(digits)));
    while (length > 0) {
        seL4_DebugPutChar(digits[--length]);
    }
}

void debug_write_hex(uint64_t value) noexcept
{
    debug_write("0x");
    bool leading = true;
    for (int shift = 60; shift >= 0; shift -= 4) {
        auto digit = static_cast<unsigned>(value >> shift) & 0xfu;
        if (digit == 0 && leading && shift != 0) {
            continue;
        }
        leading = false;
        seL4_DebugPutChar(static_cast<char>(digit < 10 ? ('0' + digit) : ('a' + digit - 10)));
    }
}

[[noreturn]] void halt() noexcept
{
    /* Park by suspending: suspended, a thread is out of the scheduler
     * entirely, where a yield loop stays runnable at its own priority
     * forever -- seL4_Yield reaches equal priorities only, so a parked
     * service would starve every thread below it (a session runs below
     * the boot set). Every process holds its own TCB at slot 1
     * (aegir/bootstrap.h, kSlotOwnTcb), so the cap is always at hand. */
    seL4_TCB_Suspend(seL4_CapInitThreadTCB);
    /* A suspend that returned did not take; halt() does not return. */
    for (;;) {
        seL4_Yield();
    }
}

}  // namespace aegir
