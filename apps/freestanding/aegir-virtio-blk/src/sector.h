/*
 * The block device's own shapes over the shared queue: the one request this
 * driver makes, and the page areas it lives in.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The queue's rings are the library's (aegir/virtio/queue.h); what is
 * blk-specific is the request header the device reads, the sector of data, and
 * the status byte, laid out in the queue page past the available ring, so that
 * a single 4 KiB page holds all of it.
 */

#pragma once

#include <aegir/virtio/queue.h>
#include <stdint.h>

namespace aegir::virtio {

/* The blk areas of the queue page, in bytes from its start: past the rings,
 * inside the first page. */
constexpr uint32_t kRequestOffset = 512;
constexpr uint32_t kDataOffset = kRequestOffset + 16;                         /* 528 */
constexpr uint32_t kStatusOffset = kDataOffset + 512;                         /* 1040 */
constexpr uint32_t kSectorBytes = 512;

/* The block device's request header (virtio 1.x, 5.2.6): a type, a reserved word, and the
 * 512-byte sector to start at. `type` 0 is a read into the data buffer, 1 a write from it,
 *  and 11 a discard (5.2.6.2), whose range travels in a segment array instead of the header's
 * sector. */
constexpr uint32_t kBlkTypeIn = 0;
constexpr uint32_t kBlkTypeOut = 1;
constexpr uint32_t kBlkTypeDiscard = 11;
constexpr uint32_t kBlkOk = 0;
constexpr uint32_t kBlkUnsupp = 2;

/* The feature bit that promises discard, and the config fields that bound one
 * (virtio 1.x, 5.2.5 and 5.3): the largest range in sectors, how many segments
 * one request may carry, and the alignment a range's first sector must keep.
 * The config offsets are from `kConfig` in mmio.h. */
constexpr uint32_t kBlkFeatureDiscard = 1u << 13;
constexpr uint32_t kConfigMaxDiscardSectors = 36;
constexpr uint32_t kConfigMaxDiscardSeg = 40;
constexpr uint32_t kConfigDiscardAlignment = 44;

struct BlkRequest {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

/** One discard range (virtio 1.x, 5.2.6.2): the start sector, the count, and a
 *  flags word that is zero for a discard (the write-zeroes form sets bits). */
struct DiscardSegment {
    uint64_t sector;
    uint32_t num_sectors;
    uint32_t flags;
};

/** What one transfer produced: the queue's answer, plus the device's own
 *  status byte from the page -- read whether or not the device answered,
 *  because on a timeout the sentinel is the evidence that it never looked. */
struct ReadResult {
    bool completed;   /* the device published a used entry before the poll ran out */
    uint32_t status;  /* the device's own status byte: 0 is OK, 2 is unsupported */
    uint32_t used_bytes;
    uint32_t used_flags;
    uint32_t used_idx;
    uint32_t device_status;
    uint32_t interrupt_status;
};

/** Publish a read of `sector` and wait for the device to say it is done. The device writes
 *  the sector to `data_physical` -- which does not have to be the queue page's own data
 *  area: the shared window a block port serves through is a physical address the driver
 *  knows, and a read that lands there needs no copy (aegir/block.h). `data_out`, when
 *  given, is filled from the queue page's own data area -- so it is for reads whose
 *  `data_physical` *is* that area, and nullptr for reads that landed somewhere else. */
ReadResult read_sector(Registers const &registers, Queue &queue, uint64_t sector,
                       uint64_t data_physical, uint8_t *data_out) noexcept;

/** The write half: the device reads the sector *from* `data_physical` -- the
 *  shared window again, so what a client left there crosses no message. The
 *  result is read_sector's: the same publish, the same wait, the same status
 *  byte. */
ReadResult write_sector(Registers const &registers, Queue &queue, uint64_t sector,
                        uint64_t data_physical) noexcept;

/** Discard `sectors` sectors from `sector`: one discard request whose range
 *  travels in a segment the driver writes into the queue page, since a discard
 *  carries no data. `max_sectors` is the device's own bound (the config's
 *  max_discard_sectors); a request larger than it is the caller's to split.
 *  The result is read_sector's. */
ReadResult discard_sectors(Registers const &registers, Queue &queue, uint64_t sector,
                           uint32_t sectors) noexcept;

}  // namespace aegir::virtio
