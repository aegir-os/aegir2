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
 * Everything machine-specific is read, not compiled in: the MAC, the link
 * state and the MTU come from the device's config space. The device has two
 * queues -- receive first, transmit second -- and the receive queue is primed
 * with one device-writable buffer per descriptor, so the device always has
 * somewhere to put a frame.
 *
 * This is the first slice: the handshake, the config space, both queues set up
 * and the receive queue primed, and the port's `info` answer. The frame half
 * (`send`, `receive`, and the client's transfer window) joins it next.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ethernet.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
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
 * full frame plus the device header, on a stride so each starts aligned. */
constexpr uint32_t kTxQueueOffset = aegir::virtio::kQueueBytes;
constexpr uint32_t kRxBuffersOffset = 2 * aegir::virtio::kQueueBytes;
constexpr uint32_t kRxBufferStride =
    (aegir::virtio::net::kReceiveBufferBytes + 63u) & ~63u;
/* The transmit self-test's buffer, past every receive buffer: the device
 * header (zeroed) then a frame, in one descriptor. */
constexpr uint32_t kTxTestOffset = kRxBuffersOffset + aegir::virtio::kQueueSize * kRxBufferStride;

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

    /* The queue memory and its physical base: a descriptor's address is a
     * machine address, and a capability does not say where it is
     * (specs/services.md). */
    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    if (!aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address)) {
        write_line("FAIL", "no memory was given to me");
        return 0;
    }

    /* The MAC is what the stack needs to become a station on the wire; the link
     * state tells it whether the wire is there. No offload features are asked
     * for. */
    uint32_t features = 0;
    uint32_t const wanted = aegir::virtio::net::kFeatureMac |
                            aegir::virtio::net::kFeatureStatus |
                            aegir::virtio::net::kFeatureMtu;
    if (!aegir::virtio::handshake(registers, &features, wanted)) {
        write_line("FAIL", "the device refused the features we asked for");
        return 0;
    }

    /* The config space, read a byte at a time: the MAC is six bytes and the
     * status sits at offset 6, so a 32-bit load would be unaligned on RISC-V.
     * The MTU is present only when the device offered the feature. */
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

    /* The two queues, before DRIVER_OK: the status bit says everything is
     * ready, and a queue set up after it may be ignored (virtio 1.x, 2.1.1 step
     * 8). The pages are zeroed first -- a nonzero used index in reused memory
     * reads as an answer that never happened. */
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
    transmitq.set_up(registers, aegir::virtio::net::kTransmitQueue, aegir::virtio::kQueueSize,
                     nullptr);

    registers.write(aegir::virtio::kStatus,
                    aegir::virtio::kStatusAcknowledge | aegir::virtio::kStatusDriver |
                        aegir::virtio::kStatusFeaturesOk | aegir::virtio::kStatusDriverOk);

    /* Prime the receive queue: one device-writable buffer per descriptor the
     * device kept, each big enough for the header and a full frame, so the
     * device has somewhere to put every frame from the first one on. */
    uint64_t const buffers_physical = memory_physical + kRxBuffersOffset;
    for (uint32_t i = 0; i < receiveq.size(); ++i) {
        aegir::virtio::ChainBuf const buffer[] = {
            {buffers_physical + i * kRxBufferStride, kRxBufferStride, true},
        };
        receiveq.publish(registers, static_cast<uint16_t>(i), buffer, 1);
    }

    /* A transmit self-test, before ready: one frame built in the queue page and
     * sent, so the transmit queue is proven end to end -- the device reads the
     * chain and posts a used entry whether or not the frame reaches anywhere
     * useful. It is a broadcast with a local-experimental ethertype, so nothing
     * on the wire needs to understand it. */
    uint64_t const tx_physical = memory_physical + kTxTestOffset;
    volatile uint8_t *const tx = memory + kTxTestOffset;
    for (uint32_t i = 0; i < aegir::virtio::net::kHeaderBytes; ++i) {
        tx[i] = 0; /* the device header, ignored with no offload negotiated */
    }
    uint8_t *const frame = const_cast<uint8_t *>(tx + aegir::virtio::net::kHeaderBytes);
    for (uint32_t i = 0; i < 6; ++i) {
        frame[i] = 0xff;       /* broadcast destination */
        frame[6 + i] = mac[i]; /* our source */
    }
    frame[12] = 0x88;
    frame[13] = 0xb5; /* local experimental ethertype */
    char const greeting[] = "aegir-virtio-net";
    uint32_t const frame_payload = 46;
    for (uint32_t i = 0; i < frame_payload; ++i) {
        frame[14 + i] =
            i < sizeof(greeting) - 1 ? static_cast<uint8_t>(greeting[i]) : 0;
    }
    uint32_t const frame_bytes = 14 + frame_payload;
    aegir::virtio::ChainBuf const tx_chain[] = {
        {tx_physical, aegir::virtio::net::kHeaderBytes + frame_bytes, false},
    };
    transmitq.publish(registers, 0, tx_chain, 1);
    aegir::virtio::UsedResult const tx_used = transmitq.wait_used(registers);
    aegir::debug_write("      transmit: ");
    if (tx_used.completed) {
        /* The used entry's length is what the device *wrote*; a transmit writes
         * nothing, so zero here is the success, and the used entry's arrival is
         * the proof the device read the chain. */
        aegir::debug_write("the device took the ");
        aegir::debug_write_unsigned(aegir::virtio::net::kHeaderBytes + frame_bytes);
        aegir::debug_write("-byte chain, used entry ");
        aegir::debug_write_unsigned(tx_used.head);
        aegir::debug_write("\n");
    } else {
        aegir::debug_write("no completion (device status ");
        aegir::debug_write_unsigned(tx_used.device_status);
        aegir::debug_write(")\n");
    }

    /* The interrupt the spawner paired with the device, when it did; a driver
     * that finds neither polls. One device, one interrupt line: it is the
     * receive queue's to wait on. */
    uint64_t irq_notification = 0;
    uint64_t irq_handler = 0;
    bool const has_irq =
        aegir::bootstrap::capability("irq.notify", 10, &irq_notification) &&
        aegir::bootstrap::capability("irq.handler", 11, &irq_handler);
    if (has_irq) {
        receiveq.use_interrupts(irq_notification, irq_handler);
        /* The notification is bound to this thread so one receive sees both
         * calls and the device's interrupt; without the bind a bare badge
         * never arrives and the line stays high. */
        if (seL4_TCB_BindNotification(aegir::bootstrap::kSlotOwnTcb,
                                      static_cast<seL4_CPtr>(irq_notification)) !=
            seL4_NoError) {
            write_line("FAIL", "the interrupt's notification would not bind");
            return 0;
        }
    }

    aegir::ipc::Owner port = aegir::ipc::Owner::find("port", 4);
    if (!port.valid()) {
        write_line("FAIL", "no port was given to me");
        return 0;
    }

    /* The instance name the registry gave this binding (eth.virtio0), and the
     * one line the acceptance reads: the device, its MAC and the link. */
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

    aegir::debug_write("      queue: receive ");
    aegir::debug_write_unsigned(receiveq.size());
    aegir::debug_write(" buffers primed, transmit ");
    aegir::debug_write_unsigned(transmitq.size());
    aegir::debug_write(", completion ");
    aegir::debug_write(has_irq ? "interrupt\n" : "polling\n");

    /* The MAC packed into the low 48 bits of a word, the way the link port
     * answers it (aegir/ethernet.h). */
    uint64_t const mac_word = static_cast<uint64_t>(mac[0]) |
                              (static_cast<uint64_t>(mac[1]) << 8) |
                              (static_cast<uint64_t>(mac[2]) << 16) |
                              (static_cast<uint64_t>(mac[3]) << 24) |
                              (static_cast<uint64_t>(mac[4]) << 32) |
                              (static_cast<uint64_t>(mac[5]) << 40);
    uint64_t const flags = link_up ? aegir::ethernet::kInfoLinkUp : 0;

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("virtio-net", "ready");

    /* The serve loop. This slice answers `info`; the frame methods join it with
     * the client's window. A bare badge is the bound interrupt: lower the line
     * and ack, and (once frames land) harvest. */
    for (;;) {
        uint64_t words[1];
        uint32_t count = 0;
        seL4_Word badge = 0;
        uint32_t const method = port.receive_words(words, 1, &count, &badge);
        if (badge == 0 && has_irq) {
            static_cast<void>(registers.read(aegir::virtio::kInterruptStatus));
            seL4_IRQHandler_Ack(static_cast<seL4_CPtr>(irq_handler));
            continue;
        }
        if (method == aegir::ethernet::kMethodInfo) {
            uint64_t const answer[aegir::ethernet::kInfoWords] = {mac_word, mtu, flags};
            port.reply_words(answer, aegir::ethernet::kInfoWords);
        } else {
            /* A method we do not know is a protocol version we do not speak:
             * the reply says so by saying nothing. */
            port.reply(0);
        }
    }
}
