/*
 * aegir-virtio-9p: the driver for the 9P transport.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * virtio-9p (device id 9) carries 9P messages to a host export and back: the
 * driver queues a T-message and the device answers with the R-message. It is
 * the piece that makes a host directory reachable from inside the machine,
 * which is what a run wants for moving files in and out (specs/9p.md).
 *
 * The driver is handed the transport for virtio device 9, its own queue
 * memory, and a shared window: the window is split in half, the caller leaves
 * its request in the first half and reads the reply from the second, and the
 * driver points the device's two descriptors at the halves' physical
 * addresses so nothing is copied. That is the whole of its protocol
 * (aegir/p9transport.h); what the messages *mean* is the filesystem service's,
 * which speaks to this port through the device manager's registry.
 *
 * The self-test before `ready` is a version handshake: it sends Tversion and
 * checks the Rversion that comes back, so the transport is proved end to end
 * -- queue, kick, completion and reply -- before any filesystem is built on
 * it.
 */

#include <aegir/9p.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/nmspace.h>
#include <aegir/p9transport.h>
#include <aegir/virtio/handshake.h>
#include <aegir/virtio/mmio.h>
#include <aegir/virtio/p9.h>
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

/* The shared window both halves ride in. The driver maps it; the *physical*
 * base is what the device's descriptors are pointed at, because a descriptor
 * names a machine address (specs/services.md). */
uintptr_t g_window = 0;
uint32_t g_window_bytes = 0;
uint64_t g_window_physical = 0;

/* The export's name, read from the device's config space and served through
 * `info` -- the volume a filesystem registers is named after it (specs/9p.md). */
char g_tag[aegir::virtio::kP9TagMax];
uint32_t g_tag_length = 0;

aegir::virtio::Queue g_queue;

/* One round trip: the caller has left a request of `request_length` bytes at
 * the window's start. The chain is the request (device-readable) then the
 * reply half (device-writable); the answer is the reply's length, or zero
 * when the device did not answer or the request was too large for the half. */
uint32_t round_trip(aegir::virtio::Registers const &registers, uint32_t request_length) noexcept
{
    uint32_t const request_cap = aegir::p9transport::request_bytes(g_window_bytes);
    if (request_length < aegir::p9::kHeaderBytes || request_length > request_cap) {
        return 0;
    }
    uint32_t const reply_cap = aegir::p9transport::reply_bytes(g_window_bytes);
    aegir::virtio::ChainBuf const chain[] = {
        {g_window_physical, request_length, false},
        {g_window_physical + aegir::p9transport::reply_offset(g_window_bytes), reply_cap, true},
    };
    g_queue.publish(registers, 0, chain, 2);
    aegir::virtio::UsedResult const used = g_queue.wait_used(registers);
    if (!used.completed) {
        return 0;
    }
    return used.bytes < reply_cap ? used.bytes : reply_cap;
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

    write_line("virtio-9p", "looking for my device");

    uint64_t device_address = 0;
    uint32_t device_bytes = 0;
    uint64_t device_physical = 0;
    if (!aegir::bootstrap::device(&device_address, &device_bytes, &device_physical)) {
        write_line("FAIL", "no device was given to me");
        return 0;
    }

    aegir::virtio::Registers const registers(device_address);

    /* Read before writing anything: the magic says a transport is there, and
     * the device id says whether it is the one this driver is for. */
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
    if (device_id != aegir::virtio::kDeviceId9p) {
        write_line("FAIL", "my device is not the 9P transport");
        return 0;
    }

    /* The queue's memory, whose physical base the descriptors need. */
    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    if (!aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address)) {
        write_line("FAIL", "no memory was given to me");
        return 0;
    }
    if ((1ull << memory_bits) < aegir::virtio::kQueueBytes) {
        write_line("FAIL", "my memory is too small for a queue");
        return 0;
    }

    /* The one feature that matters: the config space names the export. */
    uint32_t features = 0;
    if (!aegir::virtio::handshake(registers, &features,
                                  aegir::virtio::kP9FeatureMountTag) ||
        (features & aegir::virtio::kP9FeatureMountTag) == 0) {
        write_line("FAIL", "the device did not offer the mount tag");
        return 0;
    }

    /* The tag: a 16-bit length then the bytes, at the start of config space.
     * Read as bytes -- the config window is memory like any other -- and kept
     * in this service's own storage, because the device's config space may
     * move with the transport. */
    volatile uint8_t const *config =
        reinterpret_cast<volatile uint8_t const *>(device_address + aegir::virtio::kConfig);
    uint32_t tag_length =
        static_cast<uint32_t>(config[aegir::virtio::kP9ConfigTagLen]) |
        (static_cast<uint32_t>(config[aegir::virtio::kP9ConfigTagLen + 1]) << 8);
    if (tag_length > aegir::virtio::kP9TagMax) {
        write_line("FAIL", "the export's tag is longer than the config field");
        return 0;
    }
    for (uint32_t i = 0; i < tag_length; ++i) {
        g_tag[i] = static_cast<char>(config[aegir::virtio::kP9ConfigTag + i]);
    }
    g_tag_length = tag_length;

    /* The queue, before DRIVER_OK. The page is zeroed first: a nonzero used
     * index in reused memory reads as an answer that never happened. */
    volatile uint8_t *queue_page = reinterpret_cast<volatile uint8_t *>(memory_address);
    for (uint32_t i = 0; i < aegir::virtio::kQueueBytes; ++i) {
        queue_page[i] = 0;
    }
    g_queue.place(queue_page, memory_physical);

    aegir::virtio::QueueReport queue_report{};
    g_queue.set_up(registers, aegir::virtio::kP9QueueRequest, aegir::virtio::kQueueSize,
                   &queue_report);
    aegir::debug_write("      queue: num ");
    aegir::debug_write_unsigned(queue_report.num_back);
    aegir::debug_write(" of ");
    aegir::debug_write_unsigned(queue_report.num_max);
    aegir::debug_write(queue_report.legacy ? ", legacy\n" : ", modern\n");

    registers.write(aegir::virtio::kStatus,
                    aegir::virtio::kStatusAcknowledge | aegir::virtio::kStatusDriver |
                        aegir::virtio::kStatusFeaturesOk | aegir::virtio::kStatusDriverOk);

    /* The window the caller's messages cross through: the driver maps it, and
     * its physical base is what the descriptors are pointed at. */
    uint64_t window_address = 0;
    uint32_t window_bytes = 0;
    uint64_t window_physical = 0;
    if (!aegir::bootstrap::shared_window(&window_address, &window_bytes, &window_physical)) {
        write_line("FAIL", "no shared window was given to me");
        return 0;
    }
    if (aegir::p9transport::request_bytes(window_bytes) < aegir::p9::kHeaderBytes) {
        write_line("FAIL", "my window is too small for a message");
        return 0;
    }
    g_window = static_cast<uintptr_t>(window_address);
    g_window_bytes = window_bytes;
    g_window_physical = window_physical;

    /* The interrupt the spawner paired with the device, when it did; a driver
     * that finds neither polls. */
    uint64_t irq_notification = 0;
    uint64_t irq_handler = 0;
    bool const has_irq =
        aegir::bootstrap::capability("irq.notify", 10, &irq_notification) &&
        aegir::bootstrap::capability("irq.handler", 11, &irq_handler);
    if (has_irq) {
        g_queue.use_interrupts(irq_notification, irq_handler);
    }
    write_line("completion", has_irq ? "interrupt" : "polling");

    /* The port this driver serves, named through the bootstrap block. */
    aegir::ipc::Owner port = aegir::ipc::Owner::find(aegir::p9transport::kPortName,
                                                    aegir::p9transport::kPortNameLength);
    if (!port.valid()) {
        write_line("FAIL", "no port was given to me");
        return 0;
    }

    /* The self-test: a version handshake against the host export. The request
     * is a Tversion with the msize the window can carry and the dialect we
     * speak; the reply must be an Rversion naming a dialect we know. */
    uint8_t *const request = reinterpret_cast<uint8_t *>(g_window);
    uint32_t const msize = aegir::p9transport::request_bytes(g_window_bytes);
    aegir::p9::Writer writer(request, msize);
    writer.begin(aegir::p9::kTversion, aegir::p9::kNoTag);
    writer.put_u32(msize);
    writer.put_string(aegir::p9::kVersionL, aegir::p9::kVersionLLength);
    if (!writer.finish()) {
        write_line("FAIL", "the request did not fit the window");
        return 0;
    }
    uint32_t const reply_length = round_trip(registers, writer.size());
    uint8_t const *const reply = request + aegir::p9transport::reply_offset(g_window_bytes);
    aegir::p9::Reader reader(reply, reply_length);
    uint8_t reply_type = 0;
    uint16_t reply_tag = 0;
    if (reply_length < aegir::p9::kHeaderBytes || !reader.head(&reply_type, &reply_tag) ||
        reply_type != aegir::p9::kRversion) {
        write_line("FAIL", "the export did not answer a version handshake");
        return 0;
    }
    uint32_t const server_msize = reader.get_u32();
    uint8_t const *version = nullptr;
    uint32_t version_length = 0;
    if (!reader.get_string(&version, &version_length) || !reader.ok()) {
        write_line("FAIL", "the version reply was malformed");
        return 0;
    }
    if (server_msize == 0) {
        write_line("FAIL", "the export offered no message size");
        return 0;
    }

    /* The instance name this driver was started under (p9.virtio0), so the
     * line names the transport and not only the device class. */
    uint32_t instance_length = 0;
    char const *instance = aegir::bootstrap::name(&instance_length);
    aegir::debug_write("      ");
    if (instance != nullptr) {
        aegir::debug_write(instance, instance_length);
    } else {
        aegir::debug_write("virtio-9p");
    }
    aegir::debug_write(": ");
    aegir::debug_write(reinterpret_cast<char const *>(version), version_length);
    aegir::debug_write(", msize ");
    aegir::debug_write_unsigned(server_msize);
    aegir::debug_write(", tag ");
    aegir::debug_write(g_tag, g_tag_length);
    aegir::debug_write("\n");

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("virtio-9p", "ready");

    /* The serve loop. `info` names the export; `round-trip` carries one
     * request and its reply. Calls serialize at the endpoint, so one window
     * and one chain at a time is all the protocol needs. */
    for (;;) {
        uint64_t words[4];
        uint32_t count = 0;
        seL4_Word badge = 0;
        uint32_t const method = port.receive_words(words, 4, &count, &badge);
        if (method == aegir::p9transport::kMethodInfo) {
            uint64_t answer[1 + (aegir::p9transport::kTagMax + 7) / 8];
            uint32_t const n = aegir::nmspace::pack_string(answer, g_tag, g_tag_length,
                                                           aegir::p9transport::kTagMax);
            port.reply_words(answer, n);
        } else if (method == aegir::p9transport::kMethodRoundTrip && count == 1) {
            uint32_t const reply_bytes =
                round_trip(registers, static_cast<uint32_t>(words[0]));
            uint64_t const answer[1] = {reply_bytes};
            port.reply_words(answer, 1);
        } else {
            /* A method this version does not know is answered by saying
             * nothing (specs/services.md's versioning rule). */
            port.reply_words(nullptr, 0);
        }
    }
}
