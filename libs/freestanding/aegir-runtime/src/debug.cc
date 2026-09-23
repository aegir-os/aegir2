/*
 * Minimum console and halt.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The exit bridge lives here, not in a translation unit of its own, because a
 * static archive only pulls an object the linker has a reason to: this object
 * defines debug_write and halt, which assert.cc (itself pulled by libsel4's
 * inline assertions) references, so a constructor here is certain to run. A
 * bridge in its own file would define nothing a program references and would
 * be left out of the link -- which is exactly how it failed the first time.
 */

#include <aegir/debug.h>

#include <sel4/sel4.h>

extern "C" {
/* sel4runtime's exit hooks, and musl's run-time exit handlers, declared here
 * because sel4runtime's own header is C-only (sel4runtime/stdint.h uses
 * `_Static_assert`), the same workaround specs/userland.md records for the
 * root task. `__funcs_on_exit` walks the `__cxa_atexit`/`atexit` list that
 * sel4runtime never reaches -- it runs `__fini_array` and not libc's `exit`
 * -- and `__stdio_exit` flushes the streams. Both are musl-internal, and the
 * final link resolves them from whichever libc the target links. */
typedef void sel4runtime_exit_cb(int code);
typedef int sel4runtime_pre_exit_cb(int code);
sel4runtime_exit_cb *sel4runtime_set_exit(sel4runtime_exit_cb *cb);
sel4runtime_pre_exit_cb *sel4runtime_set_pre_exit(sel4runtime_pre_exit_cb *cb);

void __funcs_on_exit(void);
void __stdio_exit(void);
}

namespace {

void run_registered_exits() noexcept
{
    __funcs_on_exit();
    __stdio_exit();
}

/* sel4runtime calls pre_exit before it runs `__fini_array`, which is musl's
 * own order in `exit` (atexit handlers first, then fini). */
int before_exit(int code) noexcept
{
    run_registered_exits();
    return code;
}

[[noreturn]] void at_exit(int code) noexcept
{
    static_cast<void>(code);
    aegir::halt();
}

}  // namespace

/* A low priority, so the bridge is in place however early a constructor wants
 * an atexit handler; seed_musl (aegir-heap) runs at 200 and this after it. */
__attribute__((constructor(201))) void install_exit_bridge() noexcept
{
    sel4runtime_set_pre_exit(before_exit);
    sel4runtime_set_exit(at_exit);
}

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
