/*
 * aegir-virtio-blk: the driver for the block device.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * It is handed the transport director's survey found for virtio device 2 -- its own frame
 * mapped into this address space, and the physical address it sits at -- because two
 * services wanting two different devices is why the bus map exists. The manifest section
 * for this service is what asks for id 2 (manifests/services.manifest, `device_id`).
 *
 * What it does today is the virtio handshake and then read the one number that says the
 * whole path works end to end: the device's `Capacity`, in 512-byte sectors, at the start
 * of its config space. That is a promise the device has to have been talked to correctly to
 * keep -- the number is the size of the disk QEMU was given, and it is only readable once
 * the device is up (virtio 1.x, 5.2.4). No virtqueue yet; the drive is the next thing, and
 * a request needs one.
 */

#include "sector.h"

#include <aegir/block.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/virtio/handshake.h>
#include <aegir/virtio/mmio.h>
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

void write_unsigned_line(char const *label, uint64_t value) noexcept
{
    aegir::debug_write("      ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write_unsigned(value);
    aegir::debug_write("\n");
}

/* The clamp table: which badge may read which sectors, and which physical
 * window its data lands in, recorded once per badge by the badge-0 caller --
 * the device's manager -- before the child that will hold the badge exists
 * (aegir/block.h). It grows into this service's own memory past the queue, on
 * demand; when that memory is gone a clamp is refused, not silently capped. */
struct Clamp {
    uint64_t badge;
    uint64_t first;
    uint64_t sectors;
    uint64_t window_physical;
    Clamp *next;
};

Clamp *g_clamps = nullptr;
uint8_t *g_clamp_free = nullptr;
uint8_t *g_clamp_end = nullptr;

Clamp const *find_clamp(uint64_t badge) noexcept
{
    for (Clamp const *clamp = g_clamps; clamp != nullptr; clamp = clamp->next) {
        if (clamp->badge == badge) {
            return clamp;
        }
    }
    return nullptr;
}

bool record_clamp(uint64_t badge, uint64_t first, uint64_t sectors,
                  uint64_t window_physical) noexcept
{
    if (find_clamp(badge) != nullptr ||
        g_clamp_free + sizeof(Clamp) > g_clamp_end) {
        return false;
    }
    auto *clamp = reinterpret_cast<Clamp *>(g_clamp_free);
    clamp->badge = badge;
    clamp->first = first;
    clamp->sectors = sectors;
    clamp->window_physical = window_physical;
    clamp->next = g_clamps;
    g_clamps = clamp;
    g_clamp_free += sizeof(Clamp);
    return true;
}

/* Read or write, the range question is the same: badge 0 is the manager and
 * the whole device; any other badge, only inside the range recorded for it,
 * and a badge with no record gets nothing. */
bool clamp_allows_range(uint64_t badge, uint64_t first, uint32_t sectors,
                        uint64_t capacity) noexcept
{
    if (first + sectors > capacity) {
        return false;
    }
    if (badge == 0) {
        return true;
    }
    Clamp const *clamp = find_clamp(badge);
    return clamp != nullptr && first >= clamp->first &&
           first + sectors <= clamp->first + clamp->sectors;
}

/* A read or a write also has to fit the window its data crosses through; a
 * discard carries no data, so it is bounded only by the range. */
bool clamp_allows(uint64_t badge, uint64_t first, uint32_t sectors, uint64_t capacity,
                  uint32_t window_sectors) noexcept
{
    if (sectors > window_sectors) {
        return false;
    }
    return clamp_allows_range(badge, first, sectors, capacity);
}

/* The physical window a caller's data belongs in: the manager (badge 0) reads
 * through the window the driver itself was started with, every other caller
 * through the window its manager carved and recorded with its clamp
 * (aegir/block.h). A badge with no clamp is caught by clamp_allows first. */
uint64_t window_for(uint64_t badge, uint64_t manager_window) noexcept
{
    if (badge == 0) {
        return manager_window;
    }
    Clamp const *clamp = find_clamp(badge);
    return clamp != nullptr ? clamp->window_physical : 0;
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

    write_line("virtio-blk", "looking for my device");

    uint64_t device_address = 0;
    uint32_t device_bytes = 0;
    uint64_t device_physical = 0;
    if (!aegir::bootstrap::device(&device_address, &device_bytes, &device_physical)) {
        write_line("FAIL", "no device was given to me");
        return 0;
    }

    aegir::virtio::Registers const registers(device_address);

    /* Read before writing anything: the magic says a transport is really there, and the
     * device id says whether it is the one this driver is for. A service handed the wrong
     * device should find that out here rather than by driving it. */
    uint32_t const magic = registers.read(aegir::virtio::kMagicValue);
    uint32_t const version = registers.read(aegir::virtio::kVersion);
    uint32_t const device_id = registers.read(aegir::virtio::kDeviceId);

    aegir::debug_write("      my device at ");
    aegir::debug_write_hex(device_address);
    aegir::debug_write(" (");
    aegir::debug_write_hex(device_physical);
    aegir::debug_write("): magic ");
    aegir::debug_write_hex(magic);
    aegir::debug_write(" (");
    aegir::debug_write(magic == aegir::virtio::kMagic ? "a virtio transport" : "not virtio");
    aegir::debug_write("), version ");
    aegir::debug_write_unsigned(version);
    aegir::debug_write(", device id ");
    aegir::debug_write_unsigned(device_id);
    aegir::debug_write("\n");

    if (magic != aegir::virtio::kMagic) {
        write_line("FAIL", "my device is not a virtio transport");
        return 0;
    }
    if (device_id != aegir::virtio::kDeviceIdBlock) {
        write_line("FAIL", "my device is not the block device");
        return 0;
    }

    /* The memory director carved for us, if we asked for any. The physical base is the whole
     * reason this exists: a virtqueue's descriptor entries are guest-physical addresses that
     * the *device* reads, and no invocation can tell a service where its own memory is
     * (specs/services.md). */
    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    if (!aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address)) {
        write_line("my memory", "the block did not say where it is");
    } else {
        aegir::debug_write("      my memory: ");
        aegir::debug_write_unsigned(1ull << memory_bits);
        aegir::debug_write(" bytes at ");
        aegir::debug_write_hex(memory_address);
        aegir::debug_write(" (physical ");
        aegir::debug_write_hex(memory_physical);
        aegir::debug_write(")\n");
    }

    uint32_t features = 0;
    if (!aegir::virtio::handshake(registers, &features,
                                  aegir::virtio::kBlkFeatureDiscard)) {
        write_line("FAIL", "the device refused the features we asked for");
        return 0;
    }
    write_unsigned_line("features", features);

    /* The number everything so far was for. `Capacity` is the first field of the block
     * device's config space: a 64-bit count of 512-byte sectors (virtio 1.x, 5.2.4). */
    uint64_t const capacity = registers.read64(aegir::virtio::kConfig);
    aegir::debug_write("      capacity: ");
    aegir::debug_write_unsigned(capacity);
    aegir::debug_write(" sectors (");
    aegir::debug_write_unsigned((capacity * 512) / 1024);
    aegir::debug_write(" KiB, ");
    aegir::debug_write_unsigned((capacity * 512) / (1024 * 1024));
    aegir::debug_write(" MiB)\n");

    /* The discard bounds, when the device took the feature. A discard's range
     * lives in a segment array (kMethodDiscard), so the two numbers that matter
     * are how large one range may be and how many segments a request may
     * carry; this driver sends one segment, so a device that allows none is a
     * device without discard. The alignment says where a range may start. */
    aegir::block::BlockCaps caps{};
    uint32_t discard_alignment = 0;
    if ((features & aegir::virtio::kBlkFeatureDiscard) != 0) {
        uint32_t const max_sectors =
            registers.read(aegir::virtio::kConfig +
                           aegir::virtio::kConfigMaxDiscardSectors);
        uint32_t const max_segments =
            registers.read(aegir::virtio::kConfig + aegir::virtio::kConfigMaxDiscardSeg);
        discard_alignment =
            registers.read(aegir::virtio::kConfig + aegir::virtio::kConfigDiscardAlignment);
        if (max_segments >= 1 && max_sectors > 0) {
            caps.flags |= aegir::block::kCapsDiscard;
            caps.max_discard_sectors = max_sectors;
        }
    }
    aegir::debug_write("      discard: ");
    aegir::debug_write((caps.flags & aegir::block::kCapsDiscard) != 0 ? "yes" : "no");
    aegir::debug_write(", max ");
    aegir::debug_write_unsigned(caps.max_discard_sectors);
    aegir::debug_write(" sectors, alignment ");
    aegir::debug_write_unsigned(discard_alignment);
    aegir::debug_write("\n");

    /* The device's own status is read for FAILED below, once DRIVER_OK has actually been
     * written -- after the queue is set up. */
    if (registers.read(aegir::virtio::kStatus) & aegir::virtio::kStatusFailed) {
        write_line("FAIL", "the device reported failure");
        return 0;
    }

    /* The queue, before anything can be put in one. A legacy transport describes its queue
     * with a different set of registers than a modern one, and this machine reports version
     * 1 -- so these reads are the check that the layout is what the driver thinks it is. The
     * queue's size and the alignment it wants are the two numbers a queue's memory has to be
     * laid out with; a wrong offset would read 0 for both and say so here rather than in a
     * request that never comes back (virtio 1.x, 4.2.2, and the ring layout itself in
     * projects/util_libs/libvirtio/include/virtio/virtio_ring.h). */
    /* What the device kept of the agreement. If bit 32 reads back set, the device took
     * VIRTIO_F_VERSION_1 and the modern interface is available to the driver; if it did not,
     * something about the feature registers themselves is wrong -- and which of those it is
     * decides whether the modern queue registers are even worth writing. */
    registers.write(aegir::virtio::kDeviceFeaturesSel, 1);
    uint32_t const features_high_back = registers.read(aegir::virtio::kDeviceFeatures);
    registers.write(aegir::virtio::kDriverFeaturesSel, 1);
    uint32_t const driver_high_back = registers.read(aegir::virtio::kDriverFeatures);
    registers.write(aegir::virtio::kDriverFeaturesSel, 0);
    registers.write(aegir::virtio::kDeviceFeaturesSel, 0);
    write_unsigned_line("features wanted: bit 32 of", 1);
    write_unsigned_line("  device says it offers (high)", features_high_back);
    write_unsigned_line("  driver features read back (high)", driver_high_back);
    write_unsigned_line("status after the handshake", registers.read(aegir::virtio::kStatus));

    /* The queue is set up *before* DRIVER_OK: the status bit says everything is ready, and a
     * device told "go" before its queue exists may ignore the queue entirely (virtio 1.x,
     * 2.1.1 step 8). This is where the modern and legacy layouts are sorted out, and the
     * report says which the device took. */
    /* Zero the queue's page before anything is put in it -- the specification's step four, and
     * Linux does it with __GFP_ZERO for the same reason: a device reads the rings' own words,
     * indices in particular, so memory that happens to hold a nonzero used index is memory
     * that makes a driver believe the device has already answered. */
    static aegir::virtio::Queue queue;
    volatile uint8_t *queue_page = reinterpret_cast<volatile uint8_t *>(memory_address);
    for (uint32_t i = 0; i < aegir::virtio::kQueueBytes; ++i) {
        queue_page[i] = 0;
    }
    queue.place(queue_page, memory_physical);
    /* The clamp table's room: everything this service's memory holds past
     * the queue. */
    g_clamp_free = reinterpret_cast<uint8_t *>(memory_address) + aegir::virtio::kQueueBytes;
    g_clamp_end = reinterpret_cast<uint8_t *>(memory_address) + (1ull << memory_bits);

    aegir::virtio::QueueReport queue_report{};
    queue.set_up(registers, 0, aegir::virtio::kQueueSize, &queue_report);
    aegir::debug_write("      queue: num ");
    aegir::debug_write_unsigned(queue_report.num_back);
    aegir::debug_write(" of ");
    aegir::debug_write_unsigned(queue_report.num_max);
    aegir::debug_write(", ready ");
    aegir::debug_write_unsigned(queue_report.ready_back);
    aegir::debug_write(", desc ");
    aegir::debug_write_hex(queue_report.desc_back);
    aegir::debug_write(", ");
    aegir::debug_write(queue_report.legacy ? "legacy, pfn " : "modern");
    if (queue_report.legacy) {
        aegir::debug_write_hex(queue_report.pfn_back);
        aegir::debug_write(" (was ");
        aegir::debug_write_hex(queue_report.pfn_before);
        aegir::debug_write(")");
    }
    aegir::debug_write("\n");

    /* Only now is the device told the driver is ready. */
    registers.write(aegir::virtio::kStatus,
                    aegir::virtio::kStatusAcknowledge | aegir::virtio::kStatusDriver |
                        aegir::virtio::kStatusFeaturesOk | aegir::virtio::kStatusDriverOk);

    /* The port this driver serves, and the shared window its answers cross
     * through: the spawner made both, and the block says where they are
     * (aegir/block.h). */
    aegir::ipc::Owner port = aegir::ipc::Owner::find("port", 4);
    if (!port.valid()) {
        write_line("FAIL", "no port was given to me");
        return 0;
    }
    uint64_t window_address = 0;
    uint32_t window_bytes = 0;
    uint64_t window_physical = 0;
    if (!aegir::bootstrap::shared_window(&window_address, &window_bytes, &window_physical)) {
        write_line("FAIL", "no shared window was given to me");
        return 0;
    }

    /* Who this device is, said by the driver rather than assigned: the unit is
     * the number in the instance name -- blk.virtio0 is unit 0 -- and the
     * public block-device name is "BD" with that unit (specs/services.md). */
    aegir::block::Identify identify{};
    identify.name[0] = 'B';
    identify.name[1] = 'D';
    identify.name[2] = '0';
    {
        uint32_t instance_length = 0;
        char const *instance = aegir::bootstrap::name(&instance_length);
        if (instance != nullptr) {
            uint32_t start = instance_length;
            while (start > 0 && instance[start - 1] >= '0' && instance[start - 1] <= '9') {
                --start;
            }
            uint32_t const digits = instance_length - start;
            if (digits > 0 && digits <= sizeof(identify.name) - 2) {
                for (uint32_t i = 0; i < digits; ++i) {
                    identify.name[2 + i] = instance[start + i];
                }
            }
        }
    }
    identify.sector_count = capacity;
    identify.sector_size = aegir::virtio::kSectorBytes;
    identify.window_sectors = window_bytes / aegir::virtio::kSectorBytes;
    uint32_t bd_length = 0;
    while (bd_length < sizeof(identify.name) && identify.name[bd_length] != '\0') {
        ++bd_length;
    }
    aegir::debug_write("      I am ");
    aegir::debug_write(identify.name, bd_length);
    aegir::debug_write(": window of ");
    aegir::debug_write_unsigned(window_bytes / 1024);
    aegir::debug_write(" KiB at ");
    aegir::debug_write_hex(window_address);
    aegir::debug_write(" (physical ");
    aegir::debug_write_hex(window_physical);
    aegir::debug_write(")\n");

    /* The interrupt the spawner paired with the device, when it did: a
     * notification to wait on after each kick, and the handler to ack after
     * each signal. A driver that finds neither polls -- virtio promises
     * progress without one, and the queue keeps its bound for that case. */
    uint64_t irq_notification = 0;
    uint64_t irq_handler = 0;
    bool const has_irq =
        aegir::bootstrap::capability("irq.notify", 10, &irq_notification) &&
        aegir::bootstrap::capability("irq.handler", 11, &irq_handler);
    if (has_irq) {
        queue.use_interrupts(irq_notification, irq_handler);
    }
    write_line("completion", has_irq ? "interrupt" : "polling");

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("virtio-blk", "ready");

    /* The serve loop. A request arrives as a method and words; its data
     * crosses through the caller's window -- identify writes its answer there,
     * and a read DMAs straight into it, because a window's physical base is an
     * address the device can be pointed at. Each caller has its own window
     * (aegir/block.h), so a caller that is preempted between its call and its
     * consumption still finds its own data when it resumes. */
    for (;;) {
        uint64_t words[4];
        uint32_t count = 0;
        seL4_Word badge = 0;
        uint32_t const method = port.receive_words(words, 4, &count, &badge);
        if (method == aegir::block::kMethodIdentify) {
            /* Identify is the device manager's call, badge 0; it answers into
             * the window the driver itself was started with, which is the one
             * the manager maps. */
            *reinterpret_cast<aegir::block::Identify *>(window_address) = identify;
            port.reply(sizeof(aegir::block::Identify));
        } else if (method == aegir::block::kMethodRead && count == 1) {
            uint64_t const first = aegir::block::read_first(words[0]);
            uint32_t const sectors = aegir::block::read_count(words[0]);
            /* A refused read answers zero with the window untouched. */
            uint32_t done = 0;
            if (clamp_allows(badge, first, sectors, capacity, identify.window_sectors)) {
                uint64_t const caller_window = window_for(badge, window_physical);
                for (uint32_t i = 0; i < sectors; ++i) {
                    aegir::virtio::ReadResult const result = aegir::virtio::read_sector(
                        registers, queue, first + i,
                        caller_window +
                            static_cast<uint64_t>(i) * aegir::virtio::kSectorBytes,
                        nullptr);
                    if (!result.completed || result.status != 0) {
                        break;
                    }
                    ++done;
                }
            }
            port.reply(done);
        } else if (method == aegir::block::kMethodWrite && count == 1) {
            uint64_t const first = aegir::block::read_first(words[0]);
            uint32_t const sectors = aegir::block::read_count(words[0]);
            /* The write's range question is the read's own, word for word;
             * the sectors leave the caller's window instead of landing in it. */
            uint32_t done = 0;
            if (clamp_allows(badge, first, sectors, capacity, identify.window_sectors)) {
                uint64_t const caller_window = window_for(badge, window_physical);
                for (uint32_t i = 0; i < sectors; ++i) {
                    aegir::virtio::ReadResult const result = aegir::virtio::write_sector(
                        registers, queue, first + i,
                        caller_window +
                            static_cast<uint64_t>(i) * aegir::virtio::kSectorBytes);
                    if (!result.completed || result.status != 0) {
                        break;
                    }
                    ++done;
                }
            }
            port.reply(done);
        } else if (method == aegir::block::kMethodCaps && count == 0) {
            /* An answer of two words, not a window: a caller's window is the
             * one it was started with, and only badge 0's is the driver's own
             * (aegir/block.h). A filesystem asking on mount reads these. */
            uint64_t const answer[2] = {caps.flags, caps.max_discard_sectors};
            port.reply_words(answer, 2);
        } else if (method == aegir::block::kMethodDiscard && count == 2) {
            uint64_t const first = words[0];
            uint64_t const asked = words[1];
            uint32_t done = 0;
            if ((caps.flags & aegir::block::kCapsDiscard) != 0 && asked != 0 &&
                asked <= 0xffffffffull &&
                clamp_allows_range(badge, first, static_cast<uint32_t>(asked), capacity)) {
                /* Discard only whole aligned ranges: a partial edge block stays
                 * allocated, which is what a hint is allowed to leave behind. */
                uint64_t const align = discard_alignment == 0 ? 1 : discard_alignment;
                uint64_t begin = (first + align - 1) / align * align;
                uint64_t const end = (first + asked) / align * align;
                while (begin < end) {
                    uint64_t const span = end - begin;
                    uint32_t const chunk =
                        span > caps.max_discard_sectors
                            ? caps.max_discard_sectors
                            : static_cast<uint32_t>(span);
                    aegir::virtio::ReadResult const result =
                        aegir::virtio::discard_sectors(registers, queue, begin, chunk);
                    if (!result.completed || result.status != 0) {
                        break;
                    }
                    done += chunk;
                    begin += chunk;
                }
            }
            port.reply(done);
        } else if (method == aegir::block::kMethodClamp && count == 4 && badge == 0) {
            /* A range grant and the client's window, recorded once: the
             * manager is the badge-0 caller, and the badge it names must not
             * have one yet. */
            uint64_t recorded = 0;
            if (words[0] != 0 && words[2] != 0 && words[3] != 0 &&
                words[1] + words[2] <= capacity &&
                record_clamp(words[0], words[1], words[2], words[3])) {
                recorded = 1;
            }
            port.reply(recorded);
        } else {
            /* A method we do not know is a protocol version we do not speak:
             * the reply says so by saying nothing. */
            port.reply(0);
        }
    }
}
