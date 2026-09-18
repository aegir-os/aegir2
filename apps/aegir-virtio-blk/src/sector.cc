/*
 * The block transfer -- implementation. See src/sector.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "sector.h"

namespace aegir::virtio {

namespace {

volatile uint8_t *byte_at(volatile uint8_t *page, uint32_t offset) noexcept
{
    return page + offset;
}

volatile uint32_t *word_at(volatile uint8_t *page, uint32_t offset) noexcept
{
    return reinterpret_cast<volatile uint32_t *>(const_cast<uint8_t *>(page) + offset);
}

void put_word(volatile uint32_t *words, uint32_t index, uint32_t value) noexcept
{
    words[index] = value;
}

/* One transfer of either direction: the type word says which, and the data
 * descriptor's kDescWrite follows it -- the device writes the buffer on a
 * read, reads it on a write. Everything else -- the chain, the publish, the
 * wait, the status byte -- is the same machine. The chain always starts at
 * descriptor 0: the port serializes callers, so one transfer is outstanding
 * at a time. */
ReadResult transfer_sector(Registers const &registers, Queue &queue, uint64_t sector,
                           uint64_t data_physical, uint8_t *data_out, uint32_t type) noexcept
{
    ReadResult result{false, 0, 0, 0, 0, 0, 0};
    volatile uint8_t *page = queue.page();
    uint64_t const physical = queue.physical();

    /* The request header: a read or a write, of one sector at `sector`. Written *before* it
     * is published, because the device may look as soon as it is told there is something to
     * do. */
    put_word(word_at(page, kRequestOffset + 0), 0, type);
    put_word(word_at(page, kRequestOffset + 0), 1, 0);
    volatile uint32_t *sector_words = word_at(page, kRequestOffset + 8);
    put_word(sector_words, 0, static_cast<uint32_t>(sector));
    put_word(sector_words, 1, static_cast<uint32_t>(sector >> 32));
    /* Not zero: whatever is there must be something the device has to overwrite. */
    *byte_at(page, kStatusOffset) = 0xff;

    ChainBuf const chain[] = {
        {physical + kRequestOffset, sizeof(BlkRequest), false},
        {data_physical, kSectorBytes, type == kBlkTypeIn},
        {physical + kStatusOffset, 1, true},
    };
    queue.publish(registers, 0, chain, 3);

    UsedResult const used = queue.wait_used(registers);
    result.status = *byte_at(page, kStatusOffset);
    result.used_flags = used.used_flags;
    result.used_idx = used.used_idx;
    result.used_bytes = used.bytes;
    result.device_status = used.device_status;
    result.interrupt_status = used.interrupt_status;
    if (!used.completed) {
        return result;
    }
    result.completed = true;

    if (data_out != nullptr) {
        volatile uint8_t *src = byte_at(page, kDataOffset);
        for (uint32_t i = 0; i < kSectorBytes; ++i) {
            data_out[i] = src[i];
        }
    }
    return result;
}

}  // namespace

ReadResult read_sector(Registers const &registers, Queue &queue, uint64_t sector,
                       uint64_t data_physical, uint8_t *data_out) noexcept
{
    return transfer_sector(registers, queue, sector, data_physical, data_out, kBlkTypeIn);
}

ReadResult write_sector(Registers const &registers, Queue &queue, uint64_t sector,
                        uint64_t data_physical) noexcept
{
    return transfer_sector(registers, queue, sector, data_physical, nullptr, kBlkTypeOut);
}

}  // namespace aegir::virtio
