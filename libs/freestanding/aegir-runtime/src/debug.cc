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

#include <aegir/bootstrap.h>
#include <aegir/log.h>

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

/* --- the serial's one writer (specs/console.md) ---------------------------
 *
 * The kernel's debug console takes one character a syscall, and the kernel
 * reschedules inside a syscall -- so a service that became runnable in the
 * middle of another's line wrote into the middle of it, and a line-based
 * reader saw two half-lines. The logger is the serial's one writer now: a
 * service hands it the lines it produces, and the logger -- single-threaded --
 * writes them whole. The runtime does the coalescing, so a line is a line
 * however many calls it took to say it.
 */

/* The line being assembled, handed over at its newline. The capacity is one
 * message's worth of bytes: a longer line travels as a run of messages, which
 * is the transport's own bound rather than a chosen limit. */
constexpr uint32_t kLineBytes =
    (seL4_MsgMaxLength - aegir::log::kConsoleBytesMr) * sizeof(seL4_Word);
/* Per *thread*, not per process: director's boot thread and its supervisor
 * thread both write, and one buffer shared between them can splice one thread's
 * line into the other's if they write at once. A line is assembled here and
 * handed to the logger whole, and a thread's own TLS (supervisor.cc writes its
 * image; sel4runtime zeroes the .tbss) is what makes that true for both. (The
 * fault report that read "hello  supervisor: service 24 () faulted" was a
 * different bug -- write_name writing straight to the serial, ahead of its own
 * line; supervisor.cc and main.cc say so.) */
thread_local char g_line[kLineBytes];
thread_local uint32_t g_line_at = 0;

bool same_text(char const *text, uint32_t length, char const *other,
               uint32_t other_length) noexcept
{
    if (length != other_length) {
        return false;
    }
    for (uint32_t i = 0; i < length; ++i) {
        if (text[i] != other[i]) {
            return false;
        }
    }
    return true;
}

/* The logger's port, resolved the once. Zero when this process must write the
 * serial itself: the logger (it holds the owning half of its own port), the
 * root task (which has no bootstrap block), and any service not given the
 * port. */
seL4_CPtr console_port() noexcept
{
    /* Per thread as well, so the two threads' first resolutions are not a race
     * on shared flags (both would compute the same port, but it is still a
     * race). */
    static thread_local seL4_CPtr port = 0;
    static thread_local bool resolved = false;
    if (resolved) {
        return port;
    }
    resolved = true;
    uint32_t name_length = 0;
    char const *name = aegir::bootstrap::name(&name_length);
    if (name != nullptr &&
        same_text(name, name_length, aegir::log::kWriterName,
                  aegir::log::kWriterNameLength)) {
        return port;
    }
    uint64_t slot = 0;
    if (aegir::bootstrap::capability(aegir::log::kPortName, aegir::log::kPortNameLength,
                                     &slot)) {
        port = static_cast<seL4_CPtr>(slot);
    }
    return port;
}

void console_send(char const *bytes, uint32_t length) noexcept
{
    seL4_CPtr const port = console_port();
    if (port == 0) {
        for (uint32_t i = 0; i < length; ++i) {
            seL4_DebugPutChar(bytes[i]);
        }
        return;
    }
    constexpr uint32_t kRunBytes =
        (seL4_MsgMaxLength - aegir::log::kConsoleBytesMr) * sizeof(seL4_Word);
    for (uint32_t at = 0; at < length; at += kRunBytes) {
        uint32_t const run = length - at < kRunBytes ? length - at : kRunBytes;
        uint32_t const words = (run + sizeof(seL4_Word) - 1) / sizeof(seL4_Word);
        seL4_SetMR(0, aegir::log::kMethodConsole);
        seL4_SetMR(aegir::log::kConsoleCountMr, run);
        for (uint32_t w = 0; w < words; ++w) {
            seL4_Word word = 0;
            for (uint32_t b = 0; b < sizeof(seL4_Word); ++b) {
                uint32_t const index = w * sizeof(seL4_Word) + b;
                if (index < run) {
                    word |= static_cast<seL4_Word>(
                                static_cast<unsigned char>(bytes[at + index]))
                            << (8 * b);
                }
            }
            seL4_SetMR(aegir::log::kConsoleBytesMr + w, word);
        }
        seL4_MessageInfo_t const info =
            seL4_MessageInfo_new(0, 0, 0, aegir::log::kConsoleBytesMr + words);
        (void)seL4_Call(port, info);
    }
}

void console_put(char c) noexcept
{
    g_line[g_line_at++] = c;
    if (c == '\n' || g_line_at == kLineBytes) {
        console_send(g_line, g_line_at);
        g_line_at = 0;
    }
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
    if (text == nullptr) {
        return;
    }
    for (char const *cursor = text; *cursor != '\0'; ++cursor) {
        console_put(*cursor);
    }
}

void debug_write(char const *text, uint32_t length) noexcept
{
    for (uint32_t i = 0; i < length; ++i) {
        console_put(text[i]);
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
        console_put(digits[--length]);
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
        console_put(static_cast<char>(digit < 10 ? ('0' + digit) : ('a' + digit - 10)));
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
