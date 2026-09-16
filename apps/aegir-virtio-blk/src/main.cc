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

#include "queue.h"
#include "virtio_mmio.h"

#include <aegir/bootstrap.h>
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

void write_unsigned_line(char const *label, uint64_t value) noexcept
{
    aegir::debug_write("      ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write_unsigned(value);
    aegir::debug_write("\n");
}

/** The status handshake (virtio 1.x, 2.1.1). The device is told, in order, that we have
 *  seen it, that we know how to drive it, and what features we will use; it then either
 *  accepts the feature set -- leaving FEATURES_OK set -- or clears the bit to say it will
 *  not work with us.
 *
 *  This is what a second virtio driver needs unchanged, and so is everything above it in
 *  virtio_mmio.h: the register window is the same for every device on the bus, and only
 *  the config space and the queue after it are the device's own. */
bool handshake(aegir::virtio::Registers const &registers, uint32_t *features_out) noexcept
{
    using namespace aegir::virtio;

    /* Reset first. A device someone else has been using -- or this one, last boot -- starts
     * from zero, and the spec makes the reset step one for exactly that reason. */
    registers.write(kStatus, 0);
    for (unsigned spin = 0; spin < 1000000 && registers.read(kStatus) != 0; ++spin) {
    }

    registers.write(kStatus, kStatusAcknowledge);
    registers.write(kStatus, kStatusAcknowledge | kStatusDriver);

    /* Features. A 64-bit value in a 32-bit register, low half first: the selector exists
     * only on the modern interface, and a legacy transport answers the low half and ignores
     * a selector write (virtio 1.x, 4.2.2 -- and QEMU hands out the legacy interface unless
     * asked for modern, which is why the version is read and branched on rather than
     * assumed). */
    bool const modern = registers.read(kVersion) == 2;
    uint32_t const features_low = registers.read(kDeviceFeatures);
    uint32_t features_high = 0;
    if (modern) {
        registers.write(kDeviceFeaturesSel, 1);
        features_high = registers.read(kDeviceFeatures);
        registers.write(kDeviceFeaturesSel, 0);
    }
    static_cast<void>(features_high);

    /* With one exception: VIRTIO_F_VERSION_1 is bit 32, and it is not a feature so much as the
     * handshake saying which interface we understood. It is written whether or not the version
     * register claims the modern interface, because this device's version register says 1
     * while it answers the modern register layout -- and a device that offers that layout will
     * not use a queue until the driver confirms it. Gating this on the version register is
     * exactly the assumption that left a queue set up, notified, and untouched: the status
     * byte's sentinel came back unchanged. */
    registers.write(kDriverFeatures, 0);
    registers.write(kDriverFeaturesSel, 1);
    registers.write(kDriverFeatures, 1); /* VIRTIO_F_VERSION_1 */
    registers.write(kDriverFeaturesSel, 0);

    registers.write(kStatus, kStatusAcknowledge | kStatusDriver | kStatusFeaturesOk);

    /* The device clears FEATURES_OK when it cannot live with what we asked for, so this
     * read is the check rather than a formality (virtio 1.x, 2.1.1 step 6). */
    if ((registers.read(kStatus) & kStatusFeaturesOk) == 0) {
        return false;
    }

    if (features_out != nullptr) {
        *features_out = features_low;
    }
    return true;
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
    if (!handshake(registers, &features)) {
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
    volatile uint8_t *queue_page = reinterpret_cast<volatile uint8_t *>(memory_address);
    for (uint32_t i = 0; i < aegir::virtio::kQueueBytes; ++i) {
        queue_page[i] = 0;
    }

    aegir::virtio::QueueReport queue_report{};
    aegir::virtio::set_up(registers, memory_physical, aegir::virtio::kQueueSize, &queue_report);
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

    /* Read sector 0, and see what comes back. Everything above exists for this: the page the
     * spawner mapped is written here, and the *physical* address of that same page goes into
     * the queue registers, because the device reads by physical address and the driver writes
     * by virtual one. The disk was given to QEMU as a file, so what sector 0 holds is not
     * 512 zero bytes -- and if it is, nothing actually happened. */
    uint8_t sector_data[aegir::virtio::kSectorBytes];
    aegir::virtio::ReadResult const read =
        aegir::virtio::read_sector(registers, queue_page, memory_physical, 0, sector_data);
    if (!read.completed) {
        aegir::debug_write("      nothing came back: status ");
        aegir::debug_write_unsigned(read.status);
        aegir::debug_write(", used flags ");
        aegir::debug_write_unsigned(read.used_flags);
        aegir::debug_write(", idx ");
        aegir::debug_write_unsigned(read.used_idx);
        aegir::debug_write(", len ");
        aegir::debug_write_unsigned(read.used_bytes);
        aegir::debug_write(", device status ");
        aegir::debug_write_unsigned(read.device_status);
        aegir::debug_write(" (needs-reset is ");
        aegir::debug_write_unsigned(0x40u);
        aegir::debug_write("), interrupt status ");
        aegir::debug_write_unsigned(read.interrupt_status);
        aegir::debug_write("\n");
        write_line("FAIL", "the device never answered the read");
    } else {
        aegir::debug_write("      read sector 0: status ");
        aegir::debug_write_unsigned(read.status);
        aegir::debug_write(" (0 is ok), ");
        aegir::debug_write_unsigned(read.used_bytes);
        aegir::debug_write(" bytes used, first 16: ");
        for (unsigned i = 0; i < 16; ++i) {
            aegir::debug_write_hex(sector_data[i]);
            aegir::debug_write(" ");
        }
        aegir::debug_write("\n");
    }

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("virtio-blk", "ready");
    /* A request needs a virtqueue and an interrupt or a poll loop, neither of which exists
     * yet; when there is a port to serve, this is where the loop goes. director's boot
     * thread stops waiting at the marker the same way it stops for every other service. */
    aegir::halt();
    return 0;
}
