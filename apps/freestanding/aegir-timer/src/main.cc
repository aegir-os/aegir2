/*
 * aegir-timer: the interval timer.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Serves timer.main (aegir/timer.h): a monotonic now, a sleep, and a periodic
 * subscription. On this machine it is backed by the goldfish RTC's alarm and
 * interrupt -- the only interval source in userspace -- which it shares with
 * aegir-rtc's clock.main: that service reads the counter, this one programs the
 * alarm. The device manager binds the RTC as a platform device, grants this
 * service the frame and issues its interrupt (specs/services.md,
 * specs/timer.md).
 *
 * The service waits on two sources at once -- its port and the alarm -- by
 * binding the interrupt's notification to its serving thread, so one receive
 * sees a call and a tick alike (specs/signal.md). A sleep is therefore a *held
 * reply*: the caller's reply capability is saved and answered when the alarm
 * reaches its target, which is what lets the service keep serving meanwhile.
 * Several sleeps are held at once (a held reply each, in a pool grown from the
 * service's region), because hosted processes sleep independently and a
 * refused sleep would be a silently shortened one.
 *
 * A tick must be told apart from a call. A call from a *system* command -- the
 * boot session's, say -- carries badge 0, the same as an unbadged signal, so
 * the badge alone cannot say which. So the tick's notification is a **badged
 * copy**: the interrupt handler is re-pointed at a copy minted with a context
 * bit (aegir/signal.h's convention), and a receive whose badge is exactly that
 * bit is the tick; badge 0 and any caller's badge are calls. The bit is high
 * (1<<31) so it cannot collide with a system serial (24 bits) and is out of
 * reach of a user index short of 128 users. Pointing the handler at the copy
 * is `seL4_IRQHandler_SetNotification`, which needs send rights on the
 * notification -- why the device manager grants it Write (main.cc).
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

/* goldfish-rtc's registers (QEMU's hw/rtc/goldfish_rtc.c). */
constexpr uint32_t kTimeLow = 0x00;
constexpr uint32_t kTimeHigh = 0x04;
constexpr uint32_t kAlarmLow = 0x08;
constexpr uint32_t kAlarmHigh = 0x0c;
constexpr uint32_t kIrqEnabled = 0x10;
constexpr uint32_t kClearAlarm = 0x14;
constexpr uint32_t kAlarmStatus = 0x18;
constexpr uint32_t kClearInterrupt = 0x1c;
constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;

/* The badge the tick's notification is minted with: a high context bit, so it
 * is distinct from badge 0 (a system call) and from any caller's badge. */
constexpr seL4_Word kTickBit = 1ull << 31;

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

/* Arm the alarm for `target`: the high half first, because the low-half write
 * is what arms it -- QEMU's goldfish_rtc_write calls goldfish_rtc_set_alarm
 * only there, using the whole 64-bit value (hw/rtc/goldfish_rtc.c). */
void arm(volatile uint32_t *registers, uint64_t target) noexcept
{
    registers[kAlarmHigh / 4] = static_cast<uint32_t>(target >> 32);
    registers[kAlarmLow / 4] = static_cast<uint32_t>(target & 0xffffffffu);
    registers[kIrqEnabled / 4] = 1;
}

/* A subscription: a capability to signal each period, and when it is next due.
 * A linked list, carved one node at a time from the service's region, so the
 * table grows on demand rather than to a fixed size. */
struct Subscriber {
    seL4_CPtr cap;
    uint64_t period;
    uint64_t next;
    Subscriber *link;
};

/* A held sleep: the slot its caller's reply capability was saved into, and the
 * time to answer it. */
struct Sleep {
    seL4_CPtr slot;
    uint64_t target;
    Sleep *link;
};

/* The service's region and the free list over it. */
uint8_t *g_bump = nullptr;
uint8_t *g_end = nullptr;
Sleep *g_sleep_free = nullptr;
Subscriber *g_subs = nullptr;
Sleep *g_sleeps = nullptr;

void *region_alloc(uint32_t bytes) noexcept
{
    uint32_t const aligned = (bytes + 7u) & ~7u;
    if (g_bump == nullptr || g_bump + aligned > g_end) {
        return nullptr;
    }
    void *const result = g_bump;
    g_bump += aligned;
    return result;
}

/* The held-reply slot pool: a bitmap over the CSpace slots past everything the
 * bootstrap block named. Slots are carved on demand and returned when a sleep
 * completes, so the pool reuses them. */
uint8_t *g_slot_bits = nullptr;
seL4_CPtr g_slot_base = 0;
uint32_t g_slot_count = 0;

bool slot_alloc(seL4_CPtr *out) noexcept
{
    for (uint32_t i = 0; i < g_slot_count; ++i) {
        if ((g_slot_bits[i / 8] & (1u << (i % 8))) == 0) {
            g_slot_bits[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
            *out = static_cast<seL4_CPtr>(g_slot_base + i);
            return true;
        }
    }
    return false;
}

void slot_free(seL4_CPtr slot) noexcept
{
    uint32_t const i = static_cast<uint32_t>(slot) - g_slot_base;
    if (i < g_slot_count) {
        g_slot_bits[i / 8] &= static_cast<uint8_t>(~(1u << (i % 8)));
    }
}

void reply_slot(seL4_CPtr slot, uint64_t word) noexcept
{
    seL4_SetMR(0, word);
    seL4_Send(slot, seL4_MessageInfo_new(0, 0, 0, 1));
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
    volatile uint32_t *const registers = reinterpret_cast<volatile uint32_t *>(device_address);

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

    aegir::ipc::Owner port =
        aegir::ipc::Owner::find(aegir::timer::kPortName, aegir::timer::kPortNameLength);
    if (!port.valid()) {
        write_line("FAIL", "no timer port was given to me");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        return 0;
    }

    /* The region the timer is given: its subscriber list, its held sleeps and
     * the slot bitmap all grow from it on demand. */
    uint64_t region_physical = 0;
    uint32_t region_bits = 0;
    uint64_t region_address = 0;
    if (aegir::bootstrap::untyped(&region_physical, &region_bits, &region_address)) {
        g_bump = reinterpret_cast<uint8_t *>(region_address);
        g_end = g_bump + (1ull << region_bits);
    }

    /* The slots: everything past what the bootstrap block named is ours, and
     * the first free slot is the scratch where a subscription's capability is
     * received before it is moved. */
    seL4_CPtr const root = aegir::bootstrap::kSlotOwnCNode;
    seL4_Word const depth = aegir::bootstrap::cnode_bits();
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            uint64_t const occupied =
                entry.kind == aegir::bootstrap::EntryKind::Capability ? entry.number
                : entry.kind == aegir::bootstrap::EntryKind::DeviceCapability
                    ? entry.reserved
                    : 0;
            if (occupied != 0 && occupied + 1 > first_free) {
                first_free = occupied + 1;
            }
        }
    }
    uint32_t const cnode_slots = 1u << aegir::bootstrap::cnode_bits();
    if (first_free + 1 < cnode_slots) {
        uint32_t const count = cnode_slots - static_cast<uint32_t>(first_free + 1);
        uint8_t *const bits = static_cast<uint8_t *>(region_alloc((count + 7) / 8));
        if (bits != nullptr) {
            g_slot_base = static_cast<seL4_CPtr>(first_free + 1);
            g_slot_count = count;
            g_slot_bits = bits;
        }
    }
    seL4_SetCapReceivePath(root, static_cast<seL4_CPtr>(first_free), depth);

    /* The tick's notification: a badged copy of the interrupt's, pointed at by
     * the handler and bound to this thread. A receive whose badge is exactly
     * kTickBit is the tick; badge 0 (a system call) and any caller's badge are
     * calls. The copy needs send rights for the handler to signal it. */
    seL4_CPtr tick_notification = 0;
    bool have_tick = false;
    if (notification != 0 && handler != 0 && slot_alloc(&tick_notification) &&
        seL4_CNode_Mint(root, tick_notification, depth, root, notification, depth,
                        seL4_CapRights_new(0, 0, 1, 1), kTickBit) == seL4_NoError &&
        seL4_IRQHandler_SetNotification(handler, tick_notification) == seL4_NoError &&
        seL4_TCB_BindNotification(aegir::bootstrap::kSlotOwnTcb, tick_notification) ==
            seL4_NoError) {
        have_tick = true;
    } else {
        write_line("timer", "no interrupt was given -- sleep and the tick will not wait");
    }

    uint64_t const seconds = read_nanoseconds(registers) / kNanosecondsPerSecond;
    aegir::debug_write("      timer at ");
    aegir::debug_write_hex(device_physical);
    aegir::debug_write(": ");
    aegir::debug_write_unsigned(seconds);
    aegir::debug_write(" seconds since the epoch, ");
    aegir::debug_write_unsigned(g_slot_count);
    aegir::debug_write(" reply slots\n");

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("timer", "ready");

    for (;;) {
        /* Arm for the nearest thing due: a held sleep's target or a
         * subscription's next tick. With neither, leave the alarm off. */
        uint64_t now = read_nanoseconds(registers);
        uint64_t next = 0;
        bool armed = false;
        for (Sleep const *s = g_sleeps; s != nullptr; s = s->link) {
            if (!armed || s->target < next) {
                next = s->target;
                armed = true;
            }
        }
        for (Subscriber const *s = g_subs; s != nullptr; s = s->link) {
            if (!armed || s->next < next) {
                next = s->next;
                armed = true;
            }
        }
        if (armed && have_tick) {
            arm(registers, next < now ? now : next);
        }

        seL4_Word badge = 0;
        seL4_MessageInfo_t const info = seL4_Recv(port.capability(), &badge);
        uint32_t const length = static_cast<uint32_t>(seL4_MessageInfo_get_length(info));
        now = read_nanoseconds(registers);

        if (badge == kTickBit && have_tick) {
            /* The alarm fired: clear and ack it, then answer everything that is
             * now due. */
            registers[kClearInterrupt / 4] = 1;
            seL4_IRQHandler_Ack(handler);
            for (Subscriber *s = g_subs; s != nullptr; s = s->link) {
                if (now >= s->next) {
                    seL4_Signal(s->cap);
                    /* Advance from now, not the old `next`: a period that was
                     * missed signals once, no burst (specs/signal.md). */
                    s->next = now + s->period;
                }
            }
            Sleep **slot = &g_sleeps;
            while (*slot != nullptr) {
                Sleep *const s = *slot;
                if (now >= s->target) {
                    reply_slot(s->slot, 1);
                    slot_free(s->slot);
                    *slot = s->link;
                    s->link = g_sleep_free;
                    g_sleep_free = s;
                } else {
                    slot = &s->link;
                }
            }
            continue;
        }

        uint32_t const method = static_cast<uint32_t>(seL4_GetMR(0));
        if (method == aegir::timer::kMethodNow) {
            uint64_t const answer[aegir::timer::kNowWords] = {
                now / kNanosecondsPerSecond, now % kNanosecondsPerSecond};
            seL4_SetMR(0, answer[0]);
            seL4_SetMR(1, answer[1]);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, aegir::timer::kNowWords));
        } else if (method == aegir::timer::kMethodSleep && length >= 2) {
            seL4_CPtr slot = 0;
            Sleep *const sleep = g_sleep_free != nullptr
                                     ? g_sleep_free
                                     : static_cast<Sleep *>(region_alloc(sizeof(Sleep)));
            if (sleep != nullptr && slot_alloc(&slot) &&
                seL4_CNode_SaveCaller(root, slot, depth) == seL4_NoError) {
                if (g_sleep_free != nullptr) {
                    g_sleep_free = sleep->link;
                }
                sleep->slot = slot;
                sleep->target = now + seL4_GetMR(1);
                sleep->link = g_sleeps;
                g_sleeps = sleep;
            } else {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            }
        } else if (method == aegir::timer::kMethodSubscribe && length >= 2) {
            seL4_CPtr const cap = static_cast<seL4_CPtr>(first_free);
            Subscriber *const sub =
                static_cast<Subscriber *>(region_alloc(sizeof(Subscriber)));
            seL4_CPtr dest = 0;
            if (sub != nullptr && slot_alloc(&dest) &&
                seL4_CNode_Move(root, dest, depth, root, cap, depth) == seL4_NoError) {
                sub->cap = dest;
                sub->period = seL4_GetMR(1);
                sub->next = now + sub->period;
                sub->link = g_subs;
                g_subs = sub;
                seL4_SetMR(0, 1);
            } else {
                if (dest != 0) {
                    slot_free(dest);
                }
                seL4_CNode_Delete(root, cap, depth);
                seL4_SetMR(0, 0);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else {
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
        }
    }
}
