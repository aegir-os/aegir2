/*
 * aegir-timer: the interval timer.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Serves timer.main (aegir/timer.h): a monotonic now and a sleep. On this
 * machine it is backed by the goldfish RTC's alarm and interrupt -- the only
 * interval source in userspace -- which it shares with aegir-rtc's clock.main:
 * that service reads the counter, this one programs the alarm. The device
 * manager binds the RTC as a platform device, grants this service the frame
 * and issues its interrupt (specs/services.md, specs/timer.md).
 *
 * It serves one sleep at a time: calls serialize at the port, and it blocks on
 * the interrupt while it waits (specs/timer.md).
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/timer.h>
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

/* goldfish-rtc's registers (QEMU's hw/rtc/goldfish_rtc.c). The time and alarm
 * are each one 64-bit nanosecond count split across two 32-bit halves; the
 * high half is read on both sides of the low one so a carry between them is
 * retried rather than returned a tick off. The alarm is armed by writing it
 * and enabling the interrupt, and cleared -- alarm then interrupt -- after it
 * fires. */
constexpr uint32_t kTimeLow = 0x00;
constexpr uint32_t kTimeHigh = 0x04;
constexpr uint32_t kAlarmLow = 0x08;
constexpr uint32_t kAlarmHigh = 0x0c;
constexpr uint32_t kIrqEnabled = 0x10;
constexpr uint32_t kClearAlarm = 0x14;
constexpr uint32_t kAlarmStatus = 0x18;
constexpr uint32_t kClearInterrupt = 0x1c;
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

/* Wait `duration` nanoseconds: arm the alarm, park on the interrupt, and clear
 * it. The high half is written first because the low-half write is what arms
 * the alarm -- QEMU's goldfish_rtc_write calls goldfish_rtc_set_alarm only
 * there, using the whole 64-bit value (hw/rtc/goldfish_rtc.c). The status
 * register is `alarm_running`, not "fired", so it reads zero once the alarm
 * has gone off. A spurious wake is retried. */
void sleep_for(volatile uint32_t *registers, seL4_CPtr notification,
               seL4_CPtr handler, uint64_t duration) noexcept
{
    uint64_t const target = read_nanoseconds(registers) + duration;
    registers[kAlarmHigh / 4] = static_cast<uint32_t>(target >> 32);
    registers[kAlarmLow / 4] = static_cast<uint32_t>(target & 0xffffffffu);
    registers[kIrqEnabled / 4] = 1;
    for (;;) {
        seL4_Wait(notification, nullptr);
        registers[kClearInterrupt / 4] = 1;
        seL4_IRQHandler_Ack(handler);
        if (registers[kAlarmStatus / 4] == 0 || read_nanoseconds(registers) >= target) {
            return;
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
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        return 0;
    }
    auto *const registers = reinterpret_cast<volatile uint32_t *>(device_address);

    /* The interrupt pair the device manager issued at the binding: park on the
     * notification, ack on the handler (specs/services.md). Without them a
     * sleep cannot wait, so the service reports it and serves `now` only. */
    uint64_t notify_slot = 0;
    uint64_t handler_slot = 0;
    seL4_CPtr const notification =
        aegir::bootstrap::capability("irq.notify", 10, &notify_slot)
            ? static_cast<seL4_CPtr>(notify_slot)
            : 0;
    seL4_CPtr const handler =
        aegir::bootstrap::capability("irq.handler", 11, &handler_slot)
            ? static_cast<seL4_CPtr>(handler_slot)
            : 0;
    if (notification == 0 || handler == 0) {
        write_line("timer", "no interrupt was given -- sleep will not wait");
    }

    /* The port the manifest gives this service: it owns timer.main, so the
     * owner half arrives named (specs/services.md). */
    aegir::ipc::Owner port =
        aegir::ipc::Owner::find(aegir::timer::kPortName, aegir::timer::kPortNameLength);
    if (!port.valid()) {
        write_line("FAIL", "no timer port was given to me");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        return 0;
    }

    uint64_t const seconds = read_nanoseconds(registers) / kNanosecondsPerSecond;
    aegir::debug_write("      timer at ");
    aegir::debug_write_hex(device_physical);
    aegir::debug_write(": ");
    aegir::debug_write_unsigned(seconds);
    aegir::debug_write(" seconds since the epoch\n");

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("timer", "ready");

    for (;;) {
        uint64_t words[1];
        uint32_t count = 0;
        seL4_Word badge = 0;
        static_cast<void>(badge);
        uint32_t const method = port.receive_words(words, 1, &count, &badge);
        if (method == aegir::timer::kMethodNow) {
            uint64_t const now = read_nanoseconds(registers);
            uint64_t const answer[aegir::timer::kNowWords] = {
                now / kNanosecondsPerSecond, now % kNanosecondsPerSecond};
            port.reply_words(answer, aegir::timer::kNowWords);
        } else if (method == aegir::timer::kMethodSleep) {
            uint64_t const duration = count >= 1 ? words[0] : 0;
            if (notification != 0 && handler != 0) {
                sleep_for(registers, notification, handler, duration);
            }
            uint64_t const done = 1;
            port.reply_words(&done, 1);
        } else {
            /* A method we do not know is a protocol version we do not speak:
             * the reply says so by saying nothing. */
            port.reply(0);
        }
    }
}
