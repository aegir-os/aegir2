/*
 * aegir-clock: the time source.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The machine's goldfish RTC, mapped as a device and served as the clock
 * port (aegir/clock.h). It is director's to start, because the port is a
 * static edge in the manifest and not a driver the device manager binds: the
 * one clock in the machine needs no bus map, and its consumers -- the hosted
 * runtime's clock_gettime, the FAT service's timestamps -- need a port they
 * can name, not a registry lookup (specs/services.md).
 *
 * There is no timer here: the device's alarm registers are left alone, and
 * the service answers what time it is, nothing more.
 */

#include <aegir/bootstrap.h>
#include <aegir/clock.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
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

/* goldfish-rtc's two halves of one 64-bit register. The high half is read on
 * both sides of the low one, so a carry between them is retried rather than
 * returned as a time a tick off (QEMU's hw/rtc/goldfish_rtc.c). The register
 * counts *nanoseconds* since the Unix epoch -- QEMU's goldfish_rtc_get_count
 * is qemu_clock_get_ns -- so a caller divides; the whole seconds and the
 * nanoseconds within the second are both reported. */
constexpr uint32_t kTimeLow = 0x00;
constexpr uint32_t kTimeHigh = 0x04;
constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;

uint64_t read_nanoseconds(volatile uint32_t const *registers) noexcept
{
    for (;;) {
        uint32_t const high = registers[kTimeHigh / 4];
        uint32_t const low = registers[kTimeLow / 4];
        uint32_t const again = registers[kTimeHigh / 4];
        if (high == again) {
            return (static_cast<uint64_t>(high) << 32) | low;
        }
    }
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
    } else {
        write_line("log.main port", "not given");
    }

    uint64_t device_address = 0;
    uint32_t device_bytes = 0;
    uint64_t device_physical = 0;
    if (!aegir::bootstrap::device(&device_address, &device_bytes, &device_physical) ||
        device_bytes == 0) {
        write_line("FAIL", "no rtc device was given to me");
        /* A director-supervised service that exits without signalling leaves
         * the boot waiting on it forever (there is no timer): report ready so
         * the boot finishes, and let the missing clock show as refusals. */
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        return 0;
    }
    auto const *registers = reinterpret_cast<volatile uint32_t const *>(device_address);

    uint64_t const nanoseconds = read_nanoseconds(registers);
    uint64_t const seconds = nanoseconds / kNanosecondsPerSecond;
    aegir::debug_write("      rtc at ");
    aegir::debug_write_hex(device_physical);
    aegir::debug_write(": ");
    aegir::debug_write_unsigned(seconds);
    aegir::debug_write(" seconds since the epoch\n");
    if (seconds == 0) {
        /* An epoch of zero is the device not being there, not a machine at
         * 1970: say so, but still serve -- a wrong time is better than a boot
         * that never finishes waiting for this one. */
        write_line("rtc", "reads zero -- timestamps will be wrong");
    }

    /* The port the manifest gives this service: it owns clock.main, so the
     * owner half arrives named (specs/services.md). */
    aegir::ipc::Owner port =
        aegir::ipc::Owner::find(aegir::clock::kPortName, aegir::clock::kPortNameLength);
    if (!port.valid()) {
        write_line("FAIL", "no clock port was given to me");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        return 0;
    }

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("clock", "ready");

    /* The serve loop: a `now` is two reads of the register, answered in the
     * envelope. Calls serialize at the endpoint, so one answer is all there
     * is. */
    for (;;) {
        uint64_t words[1];
        uint32_t count = 0;
        seL4_Word badge = 0;
        static_cast<void>(badge);
        uint32_t const method = port.receive_words(words, 1, &count, &badge);
        if (method == aegir::clock::kMethodNow) {
            uint64_t const now = read_nanoseconds(registers);
            uint64_t const answer[aegir::clock::kNowWords] = {
                now / kNanosecondsPerSecond, now % kNanosecondsPerSecond};
            port.reply_words(answer, aegir::clock::kNowWords);
        } else {
            /* A method we do not know is a protocol version we do not speak:
             * the reply says so by saying nothing. */
            port.reply(0);
        }
    }
}
