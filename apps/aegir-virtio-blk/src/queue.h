/*
 * The virtqueue, and the one request this driver makes.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A virtio device and its driver agree on one thing beyond the register window: a place in
 * the driver's memory where the driver says what it wants and the device says it is done.
 * That place is a virtqueue, and the whole design is in
 * projects/util_libs/libvirtio/include/virtio/virtio_ring.h -- the descriptor table, the
 * available ring, the used ring, and their two rules: the descriptor entries carry
 * *guest-physical* addresses, and each ring has a free-running index the other side never
 * writes.
 *
 * Which is why this driver needed to be told where its memory is: the device reads these
 * structures by physical address, and nothing a service can ask the kernel would say where
 * its own page is. Everything below is offsets from the start of that page, because the
 * page is the whole queue: one descriptor table, one pair of rings, one request, one sector
 * of data and one status byte, laid out so that a single 4 KiB page holds all of it.
 *
 * The queue is used by polling rather than by interrupt. virtio guarantees that a driver
 * which publishes a request makes progress without one, and an interrupt needs an IRQ
 * capability, a notification and an ack -- none of which exist yet. Polling is the honest
 * first version: it either works or it hangs, and a bound on the loop turns the second into
 * a report.
 */

#pragma once

#include "virtio_mmio.h"

#include <stdint.h>

namespace aegir::virtio {

/* One queue, eight entries. A power of two because the rings wrap by masking, and smaller
 * than the device's QueueNumMax (1024 here) because nothing needs more. */
constexpr uint32_t kQueueSize = 8;

/* The page's layout, in bytes. Three rules from the header the structures come from: the
 * descriptor table is first, the available ring follows it at 16 bytes per entry, and the
 * *used* ring starts at the next QueueAlign boundary after that -- which is what bounded
 * alignment means, and why the legacy path declares 4 rather than a page. Each ring ends with
 * the event field the *other* side reads: `used_event` after available, `avail_event` after
 * used. */
constexpr uint32_t kDescOffset = 0;
constexpr uint32_t kAvailOffset = kDescOffset + kQueueSize * 16;         /* 128 */
constexpr uint32_t kUsedOffset = 152;                                    /* align(150, 4) */
constexpr uint32_t kRequestOffset = 512;
constexpr uint32_t kDataOffset = kRequestOffset + 16;                         /* 528 */
constexpr uint32_t kStatusOffset = kDataOffset + 512;                         /* 1040 */
constexpr uint32_t kSectorBytes = 512;

/* The descriptor flags (virtio_ring.h: VRING_DESC_F_*). */
constexpr uint16_t kDescNext = 1;   /* the next descriptor continues this chain */
constexpr uint16_t kDescWrite = 2;  /* the *device* writes this buffer, not the driver */

/* The block device's request header (virtio 1.x, 5.2.6): a type, a reserved word, and the
 * 512-byte sector to start at. `type` 0 is a read into the data buffer. */
constexpr uint32_t kBlkTypeIn = 0;
constexpr uint32_t kBlkOk = 0;
constexpr uint32_t kBlkUnsupp = 2;

/** A descriptor: where a buffer is *physically*, how long it is, what to do with it, and
 *  which descriptor continues the chain. Exactly `vring_desc`. */
struct Desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

/** The available ring: what the driver has published, and the index the device reads up to.
 *  `ring[0]` is the head of the first descriptor chain. */
struct Avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[kQueueSize];
    uint16_t used_event;
};

/** The used ring: what the device has finished, and the index the driver reads up to. */
struct UsedElem {
    uint32_t id;
    uint32_t len;
};

struct Used {
    uint16_t flags;
    uint16_t idx;
    UsedElem ring[kQueueSize];
    uint16_t avail_event;
};

struct BlkRequest {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

/** What one read produced. */
struct ReadResult {
    bool completed;   /* the device published a used entry before the poll ran out */
    uint32_t status;  /* the device's own status byte: 0 is OK, 2 is unsupported */
    uint32_t used_bytes;
    /* What the device says about the queue it was given, read back after setup: a device that
     * kept nothing answers zero here, and that is worth knowing before blaming the ring. */
    uint32_t queue_num_back;
    uint32_t queue_ready_back;
    uint32_t queue_desc_back;
    uint32_t queue_num_max;
    /* True when the device kept none of the modern queue registers and the legacy shape had
     * to be used instead; `queue_pfn_back` is what it says about that. */
    bool legacy_queue;
    uint32_t queue_pfn_back;
};

constexpr uint32_t kPageBytes = 4096;

/** Set up one queue in `page`, tell the device where the page is *physically*, publish a
 *  read of `sector`, and wait for the device to say it is done.
 *
 *  `page` is the address the spawner mapped the page at, and `physical` is where that same
 *  page is in the machine -- the value the descriptor entries have to carry, because the
 *  device reads them, not the driver. Writing the page through a volatile pointer is not
 *  paranoia: the device is reading it as the driver writes. */
ReadResult read_sector(Registers const &registers, volatile uint8_t *page, uint64_t physical,
                       uint64_t sector, uint8_t *data_out) noexcept;

}  // namespace aegir::virtio
