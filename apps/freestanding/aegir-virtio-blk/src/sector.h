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
 * 512-byte sector to start at. `type` 0 is a read into the data buffer, 1 a write from it. */
constexpr uint32_t kBlkTypeIn = 0;
constexpr uint32_t kBlkTypeOut = 1;
constexpr uint32_t kBlkOk = 0;
constexpr uint32_t kBlkUnsupp = 2;

struct BlkRequest {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
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

}  // namespace aegir::virtio
