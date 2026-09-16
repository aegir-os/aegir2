/*
 * Aegir's logger: the first service, and the first port.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * It owns `log.main` (specs/services.md) and does the smallest thing a service
 * can do: wait for a call, render it, answer. Renders, not prints -- an event
 * code plus the caller's badge is a line about who did what, and the caller does
 * not have to send a string to be understood (aegir/log.h says why v1 is words).
 *
 * It reports its own readiness through the supervision notification it was given
 * (specs/director.md), and it never returns: a service that returns is a service
 * that has stopped being one, and its supervisor is the one who decides what that
 * means.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>

#include <sel4/sel4.h>

namespace {

void write(char const *text) noexcept
{
    aegir::debug_write(text);
}

void write_word(uint64_t value) noexcept
{
    aegir::debug_write_unsigned(value);
}

void render(uint32_t method, uint64_t event, seL4_Word badge) noexcept
{
    write("  log: service ");
    write_word(badge);
    write(" ");
    if (method != aegir::log::kMethodEvent) {
        write("asked for a method this version does not know: ");
        write_word(method);
        write("\n");
        return;
    }
    switch (static_cast<aegir::log::Event>(event)) {
    case aegir::log::Event::Starting:
        write("starting\n");
        return;
    case aegir::log::Event::Ready:
        write("ready\n");
        return;
    case aegir::log::Event::BootSetCreated:
        write("boot set created\n");
        return;
    }
    write("event ");
    write_word(event);
    write("\n");
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Owner port = aegir::ipc::Owner::find(aegir::log::kPortName,
                                                   aegir::log::kPortNameLength);
    if (!port.valid()) {
        write("logger: no log.main port: nothing to own\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Say we are up, then serve. The line is written by us and the readiness
     * signal is ours too: a supervisor hears "ready" and knows the port exists,
     * because a port nobody is receiving on is a port that does not answer. */
    write("  log: logger ready, owning log.main\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t event = 0;
        seL4_Word badge = 0;
        uint32_t const method = port.receive(&event, &badge);
        render(method, event, badge);
        port.reply(aegir::log::kRecorded);
    }
}
