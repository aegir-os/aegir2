/*
 * aegir-echo: the first external command (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The smallest command that proves the shell's external path end to end: it
 * inherits a copy of the shell's console stream (badged with the stream, not
 * with itself -- the shell hands its client a copy, specs/shell.md), prints
 * its arguments to it, and reports the status it finished with. It is
 * freestanding and asks for no port but the stream, so it spawns with no
 * address-space grant: the terminal retypes its objects from the spawn
 * untyped and reads its image from Initrd: through the namespace.
 *
 * A numeric first argument is its exit status, so the shell's `return code`
 * line has something non-zero to print before `exit()` carries a status
 * itself (Phase 4). The grid is the console's, so `printf` here would go to
 * the debug serial, not the window -- hence the stream calls.
 */

#include <aegir/bootstrap.h>
#include <aegir/console_stream.h>
#include <aegir/console_stream_client.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <sel4/sel4.h>

namespace {

uint32_t length_of(char const *text) noexcept
{
    uint32_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

/* The first argument as a decimal status, or zero when it is not a number. */
uint64_t status_from(char const *text) noexcept
{
    if (text[0] == '\0') {
        return 0;
    }
    uint64_t value = 0;
    for (uint32_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    return value;
}

}  // namespace

int main(int argc, char **argv)
{
    aegir::ipc::Consumer const stream = aegir::ipc::Consumer::find(
        aegir::console::kStreamPortName, aegir::console::kStreamPortNameLength);
    if (stream.valid()) {
        for (int i = 0; i < argc; ++i) {
            if (i != 0) {
                (void)aegir::console::stream_write(stream, " ", 1);
            }
            (void)aegir::console::stream_write(stream, argv[i], length_of(argv[i]));
        }
        (void)aegir::console::stream_write(stream, "\n", 1);
    } else {
        aegir::debug_write("  aegir-echo: no con.stream\n");
    }

    /* Ready first, so whoever waits on the supervision notification is not
     * left waiting while the exit call is in flight; then the status. */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    uint64_t const status = argc > 1 ? status_from(argv[1]) : 0;
    if (stream.valid()) {
        (void)aegir::console::stream_exit(stream, status);
    }
    aegir::halt();
    return 0;
}
