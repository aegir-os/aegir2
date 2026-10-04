/*
 * aegir-virtio-net: the driver for the network device.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * virtio-net, device id 1, bound from the registry row that names this binary
 * (specs/services.md). It owns the transport and serves the Ethernet link port
 * (aegir/ethernet.h): raw frames to and from the stack, which sits above it and
 * knows nothing about virtio (specs/net.md).
 *
 * Everything machine-specific is read, not compiled in: the MAC, the link state
 * and the MTU come from the device's config space. The device has two queues --
 * receive first, transmit second -- and the receive queue is primed with one
 * device-writable buffer per descriptor.
 *
 * Frame bytes never cross the message. They travel through the driver's shared
 * window (the registry row's `window` bits), which the client maps: `send`
 * transmits the frame at the window's start, and `receive` copies one there and
 * answers its length. `receive` holds its reply until the device delivers a
 * frame, so the caller waits inside its call and this thread stays free
 * (specs/signal.md).
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ethernet.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/signal.h>
#include <aegir/virtio/handshake.h>
#include <aegir/virtio/mmio.h>
#include <aegir/virtio/net.h>
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

void write_hex_byte(uint8_t value) noexcept
{
    char const digits[] = "0123456789abcdef";
    char pair[2] = {digits[(value >> 4) & 0xf], digits[value & 0xf]};
    aegir::debug_write(pair, 2);
}

/* The memory's layout, in bytes from its base: the receive queue's pages, the
 * transmit queue's, then the posted receive buffers -- one per descriptor, a
 * full frame plus the device header, on a stride so each starts aligned -- and
 * the transmit header at the end. */
constexpr uint32_t kTxQueueOffset = aegir::virtio::kQueueBytes;
constexpr uint32_t kRxBuffersOffset = 2 * aegir::virtio::kQueueBytes;
constexpr uint32_t kRxBufferStride =
    (aegir::virtio::net::kReceiveBufferBytes + 63u) & ~63u;
constexpr uint32_t kTxHeaderOffset =
    kRxBuffersOffset + aegir::virtio::kQueueSize * kRxBufferStride;

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

    write_line("virtio-net", "looking for my device");

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
    if (device_id != aegir::virtio::kDeviceIdNet) {
        write_line("FAIL", "my device is not the network device");
        return 0;
    }

    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    if (!aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address)) {
        write_line("FAIL", "no memory was given to me");
        return 0;
    }

    uint32_t features = 0;
    uint32_t const wanted = aegir::virtio::net::kFeatureMac |
                            aegir::virtio::net::kFeatureStatus |
                            aegir::virtio::net::kFeatureMtu;
    if (!aegir::virtio::handshake(registers, &features, wanted)) {
        write_line("FAIL", "the device refused the features we asked for");
        return 0;
    }

    /* The config space, read a byte at a time: the MAC is six bytes and the
     * status sits at offset 6, so a 32-bit load would be unaligned on RISC-V. */
    volatile uint8_t *const config =
        reinterpret_cast<volatile uint8_t *>(device_address + aegir::virtio::kConfig);
    uint8_t mac[aegir::ethernet::kMacBytes] = {0, 0, 0, 0, 0, 0};
    if ((features & aegir::virtio::net::kFeatureMac) != 0) {
        for (uint32_t i = 0; i < aegir::ethernet::kMacBytes; ++i) {
            mac[i] = config[aegir::virtio::net::kConfigMac + i];
        }
    }
    bool link_up = true;
    if ((features & aegir::virtio::net::kFeatureStatus) != 0) {
        uint16_t const status = static_cast<uint16_t>(
            config[aegir::virtio::net::kConfigStatus] |
            (config[aegir::virtio::net::kConfigStatus + 1] << 8));
        link_up = (status & aegir::virtio::net::kStatusLinkUp) != 0;
    }
    uint32_t mtu = aegir::ethernet::kMtuDefault;
    if ((features & aegir::virtio::net::kFeatureMtu) != 0) {
        mtu = static_cast<uint32_t>(config[aegir::virtio::net::kConfigMtu] |
                                    (config[aegir::virtio::net::kConfigMtu + 1] << 8));
    }

    /* The two queues, before DRIVER_OK. */
    static aegir::virtio::Queue receiveq;
    static aegir::virtio::Queue transmitq;
    volatile uint8_t *const memory = reinterpret_cast<volatile uint8_t *>(memory_address);
    for (uint32_t i = 0; i < 2 * aegir::virtio::kQueueBytes; ++i) {
        memory[i] = 0;
    }
    receiveq.place(memory, memory_physical);
    transmitq.place(memory + kTxQueueOffset, memory_physical + kTxQueueOffset);
    receiveq.set_up(registers, aegir::virtio::net::kReceiveQueue, aegir::virtio::kQueueSize,
                    nullptr);
    transmitq.set_up(registers, aegir::virtio::net::kTransmitQueue,
                     aegir::virtio::kQueueSize, nullptr);

    registers.write(aegir::virtio::kStatus,
                    aegir::virtio::kStatusAcknowledge | aegir::virtio::kStatusDriver |
                        aegir::virtio::kStatusFeaturesOk | aegir::virtio::kStatusDriverOk);

    /* The posted receive buffers: one device-writable buffer per descriptor, so
     * the device has somewhere to put every frame from the first one on. */
    uint64_t const buffers_physical = memory_physical + kRxBuffersOffset;
    volatile uint8_t *const buffers = memory + kRxBuffersOffset;
    for (uint32_t i = 0; i < receiveq.size(); ++i) {
        aegir::virtio::ChainBuf const buffer[] = {
            {buffers_physical + i * kRxBufferStride, kRxBufferStride, true},
        };
        receiveq.publish(registers, static_cast<uint16_t>(i), buffer, 1);
    }

    /* The transmit header: ten zero bytes in the queue page, in front of every
     * frame the device reads (no offload negotiated, so it is ignored). */
    volatile uint8_t *const tx_header = memory + kTxHeaderOffset;
    for (uint32_t i = 0; i < aegir::virtio::net::kHeaderBytes; ++i) {
        tx_header[i] = 0;
    }
    uint64_t const tx_header_physical = memory_physical + kTxHeaderOffset;

    /* Transmit one frame already at `frame_physical`: the header then the frame,
     * both device-read. The used entry's arrival proves the device read it. */
    auto transmit = [&](uint64_t frame_physical, uint32_t frame_bytes) noexcept -> bool {
        aegir::virtio::ChainBuf const chain[] = {
            {tx_header_physical, aegir::virtio::net::kHeaderBytes, false},
            {frame_physical, frame_bytes, false},
        };
        transmitq.publish(registers, 0, chain, 2);
        aegir::virtio::UsedResult const used = transmitq.wait_used(registers);
        return used.completed;
    };

    /* The shared window the frames cross through: the registry row declares it,
     * the spawner maps it, and the client maps the same frames (specs/net.md).
     * A window smaller than a frame cannot serve the port. */
    uint64_t window_address = 0;
    uint32_t window_bytes = 0;
    uint64_t window_physical = 0;
    if (!aegir::bootstrap::shared_window(&window_address, &window_bytes, &window_physical) ||
        window_bytes < aegir::ethernet::kFrameMax) {
        write_line("FAIL", "no shared window big enough for a frame");
        return 0;
    }

    /* The interrupt, bound to this thread so one receive sees calls and the
     * device's interrupt alike. */
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

    aegir::ipc::Owner port = aegir::ipc::Owner::find("port", 4);
    if (!port.valid()) {
        write_line("FAIL", "no port was given to me");
        return 0;
    }

    /* The slot a held `receive` saves its caller's reply into. */
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
    aegir::signal::Reply_holder held(aegir::bootstrap::kSlotOwnCNode,
                                     aegir::bootstrap::kCNodeBits,
                                     static_cast<seL4_CPtr>(first_free));

    uint32_t instance_length = 0;
    char const *instance = aegir::bootstrap::name(&instance_length);
    aegir::debug_write("      ");
    if (instance != nullptr) {
        aegir::debug_write(instance, instance_length);
    } else {
        aegir::debug_write("virtio-net");
    }
    aegir::debug_write(": mac ");
    for (uint32_t i = 0; i < aegir::ethernet::kMacBytes; ++i) {
        if (i != 0) {
            aegir::debug_write(":");
        }
        write_hex_byte(mac[i]);
    }
    aegir::debug_write(", mtu ");
    aegir::debug_write_unsigned(mtu);
    aegir::debug_write(", link ");
    aegir::debug_write(link_up ? "up" : "down");
    aegir::debug_write("\n");

    /* The self-test before ready: a frame built in the window and transmitted
     * through the same path `send` uses, so the transmit queue and the window
     * are proven together. */
    uint8_t *const window = reinterpret_cast<uint8_t *>(window_address);
    for (uint32_t i = 0; i < 6; ++i) {
        window[i] = 0xff;       /* broadcast destination */
        window[6 + i] = mac[i]; /* our source */
    }
    window[12] = 0x88;
    window[13] = 0xb5; /* local experimental ethertype */
    char const greeting[] = "aegir-virtio-net";
    for (uint32_t i = 0; i < 46; ++i) {
        window[14 + i] =
            i < sizeof(greeting) - 1 ? static_cast<uint8_t>(greeting[i]) : 0;
    }
    uint32_t const frame_bytes = 14 + 46;
    if (!transmit(window_physical, frame_bytes)) {
        write_line("FAIL", "the transmit self-test did not complete");
        return 0;
    }
    aegir::debug_write("      transmit: the device took the ");
    aegir::debug_write_unsigned(frame_bytes);
    aegir::debug_write("-byte frame\n");

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("virtio-net", "ready");

    uint64_t const mac_word = static_cast<uint64_t>(mac[0]) |
                              (static_cast<uint64_t>(mac[1]) << 8) |
                              (static_cast<uint64_t>(mac[2]) << 16) |
                              (static_cast<uint64_t>(mac[3]) << 24) |
                              (static_cast<uint64_t>(mac[4]) << 32) |
                              (static_cast<uint64_t>(mac[5]) << 40);
    uint64_t const flags = link_up ? aegir::ethernet::kInfoLinkUp : 0;

    /* Completed receive descriptors, in completion order. */
    struct Pending {
        uint16_t head;
        uint32_t bytes;
    };
    Pending pending[aegir::virtio::kQueueSize];
    uint32_t pending_first = 0;
    uint32_t pending_count = 0;

    auto harvest = [&]() noexcept {
        while (pending_count < aegir::virtio::kQueueSize) {
            aegir::virtio::UsedResult const used = receiveq.poll_used(registers);
            if (!used.completed) {
                return;
            }
            uint32_t const at = (pending_first + pending_count) % aegir::virtio::kQueueSize;
            pending[at].head = static_cast<uint16_t>(used.head);
            pending[at].bytes = used.bytes;
            ++pending_count;
        }
    };

    /* Copy the oldest pending frame into the window and re-prime its buffer;
     * answer its length (0 when the device wrote only the header). */
    auto take_frame = [&]() noexcept -> uint32_t {
        Pending const p = pending[pending_first];
        pending_first = (pending_first + 1) % aegir::virtio::kQueueSize;
        --pending_count;
        uint32_t const wrote =
            p.bytes > aegir::virtio::net::kHeaderBytes
                ? p.bytes - aegir::virtio::net::kHeaderBytes
                : 0;
        uint32_t const length = wrote > aegir::ethernet::kFrameMax
                                    ? aegir::ethernet::kFrameMax
                                    : wrote;
        volatile uint8_t const *const source =
            buffers + p.head * kRxBufferStride + aegir::virtio::net::kHeaderBytes;
        for (uint32_t i = 0; i < length; ++i) {
            window[i] = source[i];
        }
        aegir::virtio::ChainBuf const buffer[] = {
            {buffers_physical + p.head * kRxBufferStride, kRxBufferStride, true},
        };
        receiveq.publish(registers, p.head, buffer, 1);
        return length;
    };

    auto reply_word = [](uint64_t word) noexcept {
        seL4_SetMR(0, word);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
    };

    for (;;) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t const info = seL4_Recv(port.capability(), &badge);
        if (badge == 0 && has_irq) {
            static_cast<void>(registers.read(aegir::virtio::kInterruptStatus));
            seL4_IRQHandler_Ack(static_cast<seL4_CPtr>(irq_handler));
            harvest();
            if (held.held() && pending_count != 0) {
                held.reply(take_frame());
            }
            continue;
        }
        uint32_t const method = static_cast<uint32_t>(seL4_GetMR(0));
        if (method == aegir::ethernet::kMethodInfo) {
            uint64_t const answer[aegir::ethernet::kInfoWords] = {mac_word, mtu, flags};
            port.reply_words(answer, aegir::ethernet::kInfoWords);
        } else if (method == aegir::ethernet::kMethodSend &&
                   seL4_MessageInfo_get_length(info) >= 2) {
            uint32_t const length = static_cast<uint32_t>(seL4_GetMR(1));
            uint32_t sent = 0;
            if (length != 0 && length <= aegir::ethernet::kFrameMax &&
                transmit(window_physical, length)) {
                sent = length;
            }
            reply_word(sent);
        } else if (method == aegir::ethernet::kMethodReceive) {
            harvest();
            if (pending_count != 0) {
                reply_word(take_frame());
            } else if (!held.held() && has_irq) {
                /* Hold the reply: the caller waits inside its call, and the
                 * frame crosses when the interrupt lands. */
                if (!held.save()) {
                    reply_word(0);
                }
            } else if (!has_irq) {
                /* No interrupt to wait on: spin the ring with the queue's bound. */
                for (unsigned spin = 0; spin < 200000000 && pending_count == 0; ++spin) {
                    harvest();
                }
                reply_word(pending_count != 0 ? take_frame() : 0);
            } else {
                reply_word(0); /* one waiter at a time */
            }
        } else {
            /* A method we do not know is a protocol version we do not speak:
             * the reply says so by saying nothing. */
            static_cast<void>(port.reply(0));
        }
    }
}
