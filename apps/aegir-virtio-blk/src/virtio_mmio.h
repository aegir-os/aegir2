/*
 * virtio over MMIO: the register window a virtio transport answers on.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A virtio device on a memory-mapped bus is a page of registers, and those registers are
 * the same for every one of them: the layout below is the whole ABI (virtio 1.x, 4.2.2).
 * The bus that finds them is the device tree's `virtio,mmio` nodes, and director hands a
 * service the transport it asked for by device id, with the device's own address, because
 * identical transports answer identically to every register read.
 *
 * **This is where the second driver pays for the first.** Everything here and the status
 * handshake in `handshake()` below are what a virtio-rng or virtio-net driver needs
 * unchanged; what is blk-specific is the config layout at 0x100 and the request queue
 * after it. When that second driver exists, this header and the handshake move into a
 * library -- libs/aegir-virtio, naming to follow the rest -- and the driver keeps only its
 * own device. Until then it lives here, deliberately not duplicated in advance of the
 * evidence that would say where the seam is (specs/services.md).
 */

#pragma once

#include <stdint.h>

namespace aegir::virtio {

/* The register window. Offsets, in bytes, from the transport's base -- which is the address
 * director reports for the device, so a service's registers are `base + offset`. */
enum Offset : uint32_t {
    kMagicValue = 0x000,         /* 0x74726976, 'virt', read-only */
    kVersion = 0x004,            /* 1 = legacy interface, 2 = modern */
    kDeviceId = 0x008,           /* what is behind the transport; 0 = nothing */
    kVendorId = 0x00c,
    kDeviceFeatures = 0x010,     /* selected by DeviceFeaturesSel */
    kDeviceFeaturesSel = 0x014,
    kDriverFeatures = 0x020,
    kDriverFeaturesSel = 0x024,
    kQueueSel = 0x030,
    kQueueNumMax = 0x034,
    kQueueNum = 0x038,
    kQueueReady = 0x044,
    kQueueNotify = 0x050,
    kInterruptStatus = 0x060,
    kInterruptAck = 0x064,
    kStatus = 0x070,             /* the handshake below is all this one register */
    kQueueDescLow = 0x080,
    kQueueDescHigh = 0x084,
    kQueueDriverLow = 0x090,
    kQueueDriverHigh = 0x094,
    kQueueDeviceLow = 0x0a0,
    kQueueDeviceHigh = 0x0a4,
    kConfig = 0x100,             /* device-specific, from here to the end of the page */
};

/* The *legacy* interface's queue registers, which are not the modern ones above: a transport
 * at version 1 has no QueueDescLow/QueueDriverLow/QueueDeviceLow and describes its queue with
 * a page-aligned base instead (virtio 1.x, 4.2.2: the legacy interface). Only the offsets
 * that differ are here -- QueueNotify, InterruptStatus, InterruptACK and Status are at the
 * same offsets in both, which is why the handshake above works on either.
 *
 * These were measured before being relied on: `kLegacyQueueNumMax` reads the device's queue
 * size and `kLegacyQueueAlign` the alignment it wants, so an offset that is wrong says so
 * instead of quietly building a queue in the wrong place. */
enum LegacyOffset : uint32_t {
    kLegacyGuestPageSize = 0x028, /* the page size the driver is using; written before PFN */
    kLegacyQueueSel = 0x02c,
    kLegacyQueueNumMax = 0x030,   /* the queue's size, 0 if this selector does not exist */
    kLegacyQueueNum = 0x034,
    kLegacyQueueAlign = 0x03c,    /* the alignment the queue's memory must have */
    kLegacyQueuePfn = 0x040,      /* the queue's base, in pages of GuestPageSize */
};

/* Device status bits. Set by writing the whole value, not by setting one bit: the device
 * reads the register and acts on the change (virtio 1.x, 2.1). */
enum Status : uint32_t {
    kStatusAcknowledge = 1,      /* we have seen the device */
    kStatusDriver = 2,           /* we know how to drive it */
    kStatusDriverOk = 4,         /* everything is set up; the device may be used */
    kStatusFeaturesOk = 8,       /* we and the device agree on features */
    kStatusFailed = 128,         /* something went wrong; the device is not usable */
};

/* Device ids, as the bus numbers them. 1, 2 and 3 are in * projects/sel4_projects_libs/libsel4vmmplatsupport/include/sel4vmmplatsupport/drivers/virtio.h;
 * 4 is the entropy device, which is what QEMU's virtio-rng-device reports and what
 * director's survey measured on this machine. */
enum DeviceId : uint32_t {
    kDeviceIdNet = 1,
    kDeviceIdBlock = 2,
    kDeviceIdConsole = 3,
    kDeviceIdEntropy = 4,
};

/** The magic every virtio transport answers with, little-endian, at offset 0. */
constexpr uint32_t kMagic = 0x74726976u;

/** The register offsets an enum names are the ABI; the accessors take a plain offset so that
 *  the modern and legacy sets are both usable without pretending they are one type. */
class Registers {
public:
    explicit Registers(uintptr_t base) noexcept : base_(base) {}

    uint32_t read(uint32_t offset) const noexcept {
        return *reinterpret_cast<volatile uint32_t *>(base_ + offset);
    }

    void write(uint32_t offset, uint32_t value) const noexcept {
        *reinterpret_cast<volatile uint32_t *>(base_ + offset) = value;
    }

    /** The 64-bit config fields are two 32-bit reads, low half first (virtio 1.x, 4.2.4). */
    uint64_t read64(uint32_t offset) const noexcept {
        uint64_t const low = read(offset);
        uint64_t const high = read(offset + 4);
        return low | (high << 32);
    }

private:
    uintptr_t base_;
};

}  // namespace aegir::virtio
