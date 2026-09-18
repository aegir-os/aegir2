/*
 * aegir-virtio-input: the driver for the keyboard.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * virtio-input (device id 18), bound from the registry row that names this
 * binary (specs/services.md). The device has two queues, and that is what
 * the Queue object's per-instance state was for: the *event* queue is
 * primed with one device-writable buffer per descriptor -- the device only
 * ever writes the eight-byte event (virtio 1.x, 5.8.6.1) into a posted
 * buffer -- and the *status* queue is set up and left idle.
 *
 * Its port serves poll and next (aegir/input.h), and `next` is the first
 * port that can say "wait": when no event has arrived the reply is held --
 * the caller's reply capability is saved, and the answer crosses when the
 * interrupt lands. That is the supervisor-that-serves shape: the irq
 * notification is bound to this thread, one receive sees calls and
 * signals, and a bare badge is the signal -- a caller's badge never is,
 * because the registry's `open` minted it.
 *
 * The queue itself is the only buffer: a completed descriptor's event is
 * consumed straight out of the posted memory and the descriptor is
 * re-primed on the spot, so pending events are bounded by the queue's own
 * size and the driver holds no second queue of its own.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/input.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/virtio/handshake.h>
#include <aegir/virtio/mmio.h>
#include <aegir/virtio/queue.h>
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

/* The memory's layout, in bytes from its base: the event queue's pages, the
 * status queue's, then the posted buffers -- one per descriptor, an
 * eight-byte event in a sixteen-byte stride so each buffer starts on its
 * own line. */
constexpr uint32_t kStatusQueueOffset = aegir::virtio::kQueueBytes;
constexpr uint32_t kBuffersOffset = 2 * aegir::virtio::kQueueBytes;
constexpr uint32_t kEventBytes = 8;
constexpr uint32_t kBufferStride = 16;

/* The device config space's selectors (virtio 1.x, 5.8.4): select chooses
 * what the 128 bytes at +8 mean, subsel the page of it, and the byte at +2
 * is how much of it is real. ID_NAME is the device's own name for itself. */
constexpr uint32_t kCfgSelectName = 0x01;

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

    write_line("virtio-input", "looking for my device");

    uint64_t device_address = 0;
    uint32_t device_bytes = 0;
    uint64_t device_physical = 0;
    if (!aegir::bootstrap::device(&device_address, &device_bytes, &device_physical)) {
        write_line("FAIL", "no device was given to me");
        return 0;
    }

    aegir::virtio::Registers const registers(device_address);

    uint32_t const magic = registers.read(aegir::virtio::kMagicValue);
    uint32_t const device_id = registers.read(aegir::virtio::kDeviceId);
    if (magic != aegir::virtio::kMagic) {
        write_line("FAIL", "my device is not a virtio transport");
        return 0;
    }
    if (device_id != aegir::virtio::kDeviceIdInput) {
        write_line("FAIL", "my device is not the input device");
        return 0;
    }

    /* The queue memory and its physical base -- the buffers' addresses in the
     * descriptors are machine addresses, and a capability does not say where
     * it is (specs/services.md). */
    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    if (!aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address)) {
        write_line("FAIL", "no memory was given to me");
        return 0;
    }

    if (!aegir::virtio::handshake(registers, nullptr)) {
        write_line("FAIL", "the device refused the features we asked for");
        return 0;
    }

    /* Who this device is, said by the device: the config space's ID_NAME,
     * read a byte at a time because that is the granularity the selectors
     * work at (virtio 1.x, 5.8.4). */
    {
        volatile uint8_t *config =
            reinterpret_cast<volatile uint8_t *>(device_address + aegir::virtio::kConfig);
        config[0] = kCfgSelectName;
        config[1] = 0;
        uint32_t const size = config[2];
        aegir::debug_write("      the device says it is \"");
        for (uint32_t i = 0; i < size && i < 64; ++i) {
            char c = static_cast<char>(config[8 + i]);
            if (c == '\0') {
                break; /* the size counts the padding's NULs too */
            }
            aegir::debug_write(&c, 1);
        }
        aegir::debug_write("\"\n");
    }

    /* The two queues, before DRIVER_OK: the event queue is index 0, the
     * status queue index 1 (virtio 1.x, 5.8.2), and the status bit says
     * everything is ready, so a queue set up after it may be ignored
     * entirely (virtio 1.x, 2.1.1 step 8). The pages are zeroed first -- a
     * nonzero used index in reused memory reads as an answer that never
     * happened. */
    static aegir::virtio::Queue eventq;
    static aegir::virtio::Queue statusq;
    volatile uint8_t *const memory = reinterpret_cast<volatile uint8_t *>(memory_address);
    for (uint32_t i = 0; i < 2 * aegir::virtio::kQueueBytes; ++i) {
        memory[i] = 0;
    }
    eventq.place(memory, memory_physical);
    statusq.place(memory + kStatusQueueOffset, memory_physical + kStatusQueueOffset);
    eventq.set_up(registers, 0, aegir::virtio::kQueueSize, nullptr);
    statusq.set_up(registers, 1, aegir::virtio::kQueueSize, nullptr);

    registers.write(aegir::virtio::kStatus,
                    aegir::virtio::kStatusAcknowledge | aegir::virtio::kStatusDriver |
                        aegir::virtio::kStatusFeaturesOk | aegir::virtio::kStatusDriverOk);

    /* The buffers, posted one per descriptor: a single-entry chain each,
     * device-writable, so the device has somewhere to put every event from
     * the first keypress on. */
    uint64_t const buffers_physical = memory_physical + kBuffersOffset;
    volatile uint8_t *const buffers = memory + kBuffersOffset;
    for (uint32_t i = 0; i < aegir::virtio::kQueueSize; ++i) {
        aegir::virtio::ChainBuf const buffer[] = {
            {buffers_physical + i * kBufferStride, kBufferStride, true},
        };
        eventq.publish(registers, static_cast<uint16_t>(i), buffer, 1);
    }

    /* The port this driver serves, and the interrupt the spawner paired with
     * the device. No shared window: the events ride in the envelope
     * (aegir/input.h). */
    uint64_t port_slot = 0;
    if (!aegir::bootstrap::capability("port", 4, &port_slot)) {
        write_line("FAIL", "no port was given to me");
        return 0;
    }
    seL4_CPtr const port = static_cast<seL4_CPtr>(port_slot);
    uint64_t irq_notification = 0;
    uint64_t irq_handler = 0;
    bool const has_irq =
        aegir::bootstrap::capability("irq.notify", 10, &irq_notification) &&
        aegir::bootstrap::capability("irq.handler", 11, &irq_handler);
    if (has_irq &&
        seL4_TCB_BindNotification(aegir::bootstrap::kSlotOwnTcb,
                                  static_cast<seL4_CPtr>(irq_notification)) != seL4_NoError) {
        write_line("FAIL", "the interrupt's notification would not bind");
        return 0;
    }
    write_line("completion", has_irq ? "interrupt, bound to the serving thread" : "polling");

    /* The slot a held caller's reply capability is saved into: past
     * everything the bootstrap block names, which are ours. The slot lives in
     * a different field per entry kind -- a Capability entry's `number` is
     * its slot, a DeviceCapability's is in `reserved` (aegir/bootstrap.h:
     * `number` there is the *physical* address, and scanning it as a slot
     * lands the saved caller on the frame cap: "Destination slot not empty",
     * then a send through a page). */
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
    seL4_CPtr const held_slot = static_cast<seL4_CPtr>(first_free);

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("virtio-input", "ready");

    /* Completed descriptors, in the order the device finished them: the used
     * ring's chain heads, which are the buffer numbers. Bounded by the
     * queue's own size -- there are never more outstanding than that. */
    uint16_t pending[aegir::virtio::kQueueSize];
    uint32_t pending_first = 0;
    uint32_t pending_count = 0;

    /* Harvest what the device has completed into the pending list. */
    auto harvest = [&]() noexcept {
        while (pending_count < aegir::virtio::kQueueSize) {
            aegir::virtio::UsedResult const used = eventq.poll_used(registers);
            if (!used.completed) {
                return;
            }
            pending[(pending_first + pending_count) % aegir::virtio::kQueueSize] =
                static_cast<uint16_t>(used.head);
            ++pending_count;
        }
    };

    /* Consume the oldest pending event: read it out of its posted buffer and
     * re-prime the descriptor on the spot, so the device never runs out of
     * somewhere to write. */
    auto take_event = [&]() noexcept -> uint64_t {
        uint16_t const head = pending[pending_first];
        pending_first = (pending_first + 1) % aegir::virtio::kQueueSize;
        --pending_count;
        volatile uint8_t const *event = buffers + head * kBufferStride;
        uint16_t const type =
            static_cast<uint16_t>(event[0] | (event[1] << 8));
        uint16_t const code =
            static_cast<uint16_t>(event[2] | (event[3] << 8));
        uint32_t const value = static_cast<uint32_t>(event[4]) |
                               (static_cast<uint32_t>(event[5]) << 8) |
                               (static_cast<uint32_t>(event[6]) << 16) |
                               (static_cast<uint32_t>(event[7]) << 24);
        aegir::virtio::ChainBuf const buffer[] = {
            {buffers_physical + head * kBufferStride, kBufferStride, true},
        };
        eventq.publish(registers, head, buffer, 1);
        return aegir::input::pack_event(type, code, value);
    };

    auto reply_word = [](uint64_t word) noexcept {
        seL4_SetMR(0, word);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
    };
    auto reply_empty = []() noexcept { seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0)); };

    /* The serve loop. A bare badge is the bound notification saying the
     * device moved: lower the line, arm the next signal, harvest -- and if a
     * caller's `next` is held, its wait is over. A call is a method in the
     * first word. Without an interrupt there is no held reply to keep: a
     * `next` that finds nothing spins the ring with a bound, the queue's own
     * fallback shape. */
    bool held = false;
    for (;;) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t const info = seL4_Recv(port, &badge);
        if (badge == 0 && has_irq) {
            static_cast<void>(registers.read(aegir::virtio::kInterruptStatus));
            seL4_IRQHandler_Ack(static_cast<seL4_CPtr>(irq_handler));
            harvest();
            if (held && pending_count != 0) {
                seL4_SetMR(0, take_event());
                seL4_Send(held_slot, seL4_MessageInfo_new(0, 0, 0, 1));
                held = false;
            }
            continue;
        }
        uint32_t const method = static_cast<uint32_t>(seL4_GetMR(0));
        if (method == aegir::input::kMethodPoll) {
            harvest();
            reply_word(pending_count != 0 ? 1 : 0);
        } else if (method == aegir::input::kMethodNext) {
            harvest();
            if (pending_count != 0) {
                reply_word(take_event());
            } else if (!held && has_irq) {
                /* Hold the reply: the caller's reply capability is saved into
                 * a slot of ours -- a CNode invocation in this kernel's API,
                 * not a syscall -- and the answer crosses when the interrupt
                 * lands. */
                seL4_CNode_SaveCaller(aegir::bootstrap::kSlotOwnCNode, held_slot,
                                      aegir::bootstrap::kCNodeBits);
                held = true;
            } else if (!has_irq) {
                /* No interrupt to wait on: the ring is the only place an
                 * event can appear, so spin it with the queue's own bound. */
                for (unsigned spin = 0; spin < 200000000 && pending_count == 0; ++spin) {
                    harvest();
                }
                if (pending_count != 0) {
                    reply_word(take_event());
                } else {
                    reply_empty();
                }
            } else {
                /* One waiter at a time: a second `next` while one is held is
                 * told to try again (aegir/input.h). */
                reply_empty();
            }
        } else {
            /* A method we do not know is a protocol version we do not speak:
             * the reply says so by saying nothing. */
            reply_empty();
        }
        static_cast<void>(info);
    }
}
