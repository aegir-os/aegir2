/*
 * aegir-virtio-rng: the driver for the entropy device.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * virtio-rng, device id 4, was the device manager's own device -- the first
 * proof that a service could read hardware, made when nothing else could.
 * The proof served; the transport is on the map like any other now, bound
 * from the registry row that names this binary (specs/services.md).
 *
 * It is the smallest device on the bus: one queue, and the device only ever
 * writes into buffers the driver posts -- there is no request header and no
 * status byte, so the whole device-specific part is the buffer below. Its
 * port serves one method, `read`: one posted buffer per call, and the
 * answer rides in the envelope itself, up to what the envelope carries
 * (aegir/entropy.h) -- entropy is small, so there is no shared window.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/entropy.h>
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

/* The buffer a read posts: past the rings, inside the queue's first page,
 * and sized at what the port's envelope can carry home -- the caller's
 * ceiling and the buffer are the same number (aegir/entropy.h). 512 is past
 * the available ring's end and well short of the used ring at 4096. */
constexpr uint32_t kBufferOffset = 512;

/** One read of the device: post the buffer device-writable, wait, copy out
 *  what it wrote. The device may fill less than was posted -- the used
 *  entry's length is the truth, and zero is what a timeout answers. */
uint32_t fill_from_device(aegir::virtio::Registers const &registers,
                          aegir::virtio::Queue &queue, uint32_t bytes,
                          uint8_t *out) noexcept
{
    aegir::virtio::ChainBuf const chain[] = {
        {queue.physical() + kBufferOffset, bytes, true},
    };
    queue.publish(registers, 0, chain, 1);
    aegir::virtio::UsedResult const used = queue.wait_used(registers);
    if (!used.completed || used.bytes == 0) {
        return 0;
    }
    uint32_t const filled = used.bytes < bytes ? used.bytes : bytes;
    volatile uint8_t const *buffer = queue.page() + kBufferOffset;
    for (uint32_t i = 0; i < filled; ++i) {
        out[i] = buffer[i];
    }
    return filled;
}

/* The answer a `read` replies with, built as words because that is how it
 * crosses. Static because the envelope's worth of bytes is not a stack's
 * business -- a spawned process's stack is two pages (specs/userland.md) --
 * and the port serializes callers, so one is all there is. */
uint64_t g_answer[aegir::ipc::kMaxWords];

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

    write_line("virtio-rng", "looking for my device");

    uint64_t device_address = 0;
    uint32_t device_bytes = 0;
    uint64_t device_physical = 0;
    if (!aegir::bootstrap::device(&device_address, &device_bytes, &device_physical)) {
        write_line("FAIL", "no device was given to me");
        return 0;
    }

    aegir::virtio::Registers const registers(device_address);

    /* Read before writing anything: the magic says a transport is really there, and the
     * device id says whether it is the one this driver is for. */
    uint32_t const magic = registers.read(aegir::virtio::kMagicValue);
    uint32_t const device_id = registers.read(aegir::virtio::kDeviceId);

    aegir::debug_write("      my device at ");
    aegir::debug_write_hex(device_address);
    aegir::debug_write(" (");
    aegir::debug_write_hex(device_physical);
    aegir::debug_write("): magic ");
    aegir::debug_write_hex(magic);
    aegir::debug_write(", device id ");
    aegir::debug_write_unsigned(device_id);
    aegir::debug_write("\n");

    if (magic != aegir::virtio::kMagic) {
        write_line("FAIL", "my device is not a virtio transport");
        return 0;
    }
    if (device_id != aegir::virtio::kDeviceIdEntropy) {
        write_line("FAIL", "my device is not the entropy device");
        return 0;
    }

    /* The queue's memory, and its physical base -- the buffer's address in the
     * descriptor is a machine address, and a capability does not say where it
     * is (specs/services.md). */
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

    /* The queue, before DRIVER_OK: the status bit says everything is ready, and a
     * device told "go" before its queue exists may ignore the queue entirely
     * (virtio 1.x, 2.1.1 step 8). The page is zeroed first -- a nonzero used
     * index in reused memory reads as an answer that never happened. */
    static aegir::virtio::Queue queue;
    volatile uint8_t *queue_page = reinterpret_cast<volatile uint8_t *>(memory_address);
    for (uint32_t i = 0; i < aegir::virtio::kQueueBytes; ++i) {
        queue_page[i] = 0;
    }
    queue.place(queue_page, memory_physical);

    aegir::virtio::QueueReport queue_report{};
    queue.set_up(registers, 0, aegir::virtio::kQueueSize, &queue_report);
    aegir::debug_write("      queue: num ");
    aegir::debug_write_unsigned(queue_report.num_back);
    aegir::debug_write(" of ");
    aegir::debug_write_unsigned(queue_report.num_max);
    aegir::debug_write(queue_report.legacy ? ", legacy\n" : ", modern\n");

    registers.write(aegir::virtio::kStatus,
                    aegir::virtio::kStatusAcknowledge | aegir::virtio::kStatusDriver |
                        aegir::virtio::kStatusFeaturesOk | aegir::virtio::kStatusDriverOk);

    /* The interrupt the spawner paired with the device, when it did; a driver
     * that finds neither polls. */
    uint64_t irq_notification = 0;
    uint64_t irq_handler = 0;
    bool const has_irq =
        aegir::bootstrap::capability("irq.notify", 10, &irq_notification) &&
        aegir::bootstrap::capability("irq.handler", 11, &irq_handler);
    if (has_irq) {
        queue.use_interrupts(irq_notification, irq_handler);
    }
    write_line("completion", has_irq ? "interrupt" : "polling");

    /* The port this driver serves. No shared window: the answers ride in the
     * envelope (aegir/entropy.h). */
    aegir::ipc::Owner port = aegir::ipc::Owner::find("port", 4);
    if (!port.valid()) {
        write_line("FAIL", "no port was given to me");
        return 0;
    }

    /* The self-test, before ready is said: two reads of the device, which an
     * entropy source must answer with bytes that are neither zero nor each
     * other. */
    uint8_t first[32];
    uint8_t second[32];
    uint32_t const first_filled = fill_from_device(registers, queue, sizeof(first), first);
    uint32_t const second_filled =
        fill_from_device(registers, queue, sizeof(second), second);
    bool nonzero = false;
    bool differing = false;
    for (uint32_t i = 0; i < first_filled; ++i) {
        nonzero = nonzero || first[i] != 0;
    }
    for (uint32_t i = 0; i < first_filled && i < second_filled; ++i) {
        differing = differing || first[i] != second[i];
    }
    if (first_filled == 0 || second_filled == 0 || !nonzero || !differing) {
        write_line("FAIL", "the device would not answer with entropy");
        return 0;
    }
    aegir::debug_write("      entropy: ");
    for (unsigned i = 0; i < 8; ++i) {
        aegir::debug_write_hex(first[i]);
        aegir::debug_write(" ");
    }
    aegir::debug_write("-- and a second read differs\n");

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("virtio-rng", "ready");

    /* The serve loop. A read is one posted buffer; the answer's words are the
     * bytes the device filled, up to what the envelope carries. Calls
     * serialize at the endpoint, so one buffer is all the protocol needs. */
    for (;;) {
        uint64_t words[1];
        uint32_t count = 0;
        seL4_Word badge = 0;
        static_cast<void>(badge);
        uint32_t const method = port.receive_words(words, 1, &count, &badge);
        if (method == aegir::entropy::kMethodRead && count == 1 && words[0] != 0) {
            uint32_t const wanted = words[0] > aegir::entropy::kMaxBytes
                                        ? aegir::entropy::kMaxBytes
                                        : static_cast<uint32_t>(words[0]);
            uint32_t const filled = fill_from_device(
                registers, queue, wanted, reinterpret_cast<uint8_t *>(g_answer));
            if (filled == 0) {
                port.reply(0);
            } else {
                port.reply_words(g_answer, (filled + 7) / 8);
            }
        } else {
            /* A method we do not know is a protocol version we do not speak:
             * the reply says so by saying nothing. */
            port.reply(0);
        }
    }
}
