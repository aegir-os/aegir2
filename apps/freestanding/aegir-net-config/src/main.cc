/*
 * aegir-net-config: configure the network stack's adapters through its control
 * port (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * `NetConfig` proper is a command on `C:` that reads `Sys:S/network.manifest`
 * and applies it from `Startup-Sequence` (specs/net.md). Until the manifest's
 * reader lands, this service does what that command's first line will: open
 * `net.control`, list the adapters, and ask DHCP of each -- the numbers then
 * come from the network, never the build. The path is the real one (a client of
 * the control port); only the *source* of the configuration is a placeholder.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/netcontrol.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

void write_line(char const *label, char const *text) noexcept
{
    aegir::debug_write("      ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    aegir::ipc::Consumer const control = aegir::ipc::Consumer::find(
        aegir::netcontrol::kPortName, aegir::netcontrol::kPortNameLength);
    if (!control.valid()) {
        write_line("FAIL", "netcfg: no control port was given to me");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    aegir::ipc::Reply const count = control.call(aegir::netcontrol::kMethodList, 0);
    if (count.error != 0) {
        write_line("FAIL", "netcfg: the stack would not list its adapters");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    unsigned configured = 0;
    for (uint64_t index = 0; index < count.word; ++index) {
        uint64_t const request[3] = {index, aegir::netcontrol::kParamDhcp, 1};
        uint64_t answer[1] = {0};
        aegir::ipc::WordsReply const taken =
            control.call_words(aegir::netcontrol::kMethodSet, request, 3, answer, 1);
        if (taken.error == 0 && taken.count >= 1 && answer[0] == 1) {
            ++configured;
        }
    }
    aegir::debug_write("      netcfg: ");
    aegir::debug_write_unsigned(configured);
    aegir::debug_write(" of ");
    aegir::debug_write_unsigned(count.word);
    aegir::debug_write(" adapters asked for DHCP\n");

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("netcfg", "ready");
    aegir::halt();
}
