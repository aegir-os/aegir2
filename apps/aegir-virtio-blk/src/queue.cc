/*
 * The virtqueue's implementation: one queue, one chain of descriptors, one request.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "queue.h"

namespace aegir::virtio {

namespace {

/* The page is read and written by both sides, so every access goes through a volatile
 * pointer and the compiler is not free to reorder one past another. Everything here is a
 * small integer at a fixed offset -- a struct assignment through a volatile pointer is not
 * even well formed, and a device does not care about structs anyway. */
volatile uint8_t *byte_at(volatile uint8_t *page, uint32_t offset) noexcept
{
    return page + offset;
}

volatile uint32_t *word_at(volatile uint8_t *page, uint32_t offset) noexcept
{
    return reinterpret_cast<volatile uint32_t *>(const_cast<uint8_t *>(page) + offset);
}

volatile uint16_t *half_at(volatile uint8_t *page, uint32_t offset) noexcept
{
    return reinterpret_cast<volatile uint16_t *>(const_cast<uint8_t *>(page) + offset);
}

void put_word(volatile uint32_t *words, uint32_t index, uint32_t value) noexcept
{
    words[index] = value;
}

}  // namespace

void set_up(Registers const &registers, uint64_t physical, uint32_t num,
            QueueReport *report) noexcept
{
    QueueReport local{0, 0, 0, 0, false, 0, 0};

    /* GuestPageSize and QueueAlign come before QueueNum, and that order is the whole point.
     * The specification lists QueueNum first and QueueAlign second, but QEMU computes the
     * ring's internal offsets when it is given the size:
     *
     *     virtio_queue_set_num(vdev, sel, value);
     *     if (proxy->legacy) {
     *         virtio_queue_update_rings(vdev, sel);       // hw/virtio/virtio-mmio.c
     *     }
     *
     * and virtio_queue_update_rings returns early unless `vring->align` is already set --
     * it places the available and used rings from the alignment. Write the size first and the
     * device never computes them: `QueuePFN` then maps a ring at address zero, the device reads
     * it there, finds nothing, and does nothing at all. Which is indistinguishable, from the
     * driver's side, from a device that is not listening. */
    registers.write(kLegacyGuestPageSize, kPageBytes);

    registers.write(kQueueSel, 0);
    local.pfn_before = registers.read(kLegacyQueuePfn);
    local.num_max = registers.read(kQueueNumMax);
    registers.write(kLegacyQueueAlign, kUsedOffset);

    /* QueuePFN before QueueNum, and this one is not a preference either. `QueueNum` is what
     * makes QEMU compute the ring's layout:
     *
     *     virtio_queue_set_num(vdev, sel, value);
     *     if (proxy->legacy) {
     *         virtio_queue_update_rings(vdev, sel);       // hw/virtio/virtio-mmio.c
     *     }
     *
     * and update_rings places the available and used rings by adding to `vring->desc` -- which
     * `QueuePFN` is what sets. Write the size first and the address is still zero: the device
     * computes avail at physical 128 and used at physical 4096, then maps *those* when the
     * page frame arrives. It then reads a ring in near-zero memory, finds no requests, and
     * does nothing -- with no error and no interrupt, because from its side nothing is wrong.
     * The specification's own sequence puts QueuePFN last as the activation step; the device
     * needs it early enough to be the thing the layout is measured from. */
    registers.write(kLegacyQueuePfn, static_cast<uint32_t>(physical / kPageBytes));

    /* A queue with no size is a queue the device will not use, so the size is the smaller of
     * what the caller asked for and what the device offers. */
    uint32_t const size = local.num_max < num ? local.num_max : num;
    registers.write(kQueueNum, size);
    local.num_back = registers.read(kQueueNum);

    /* The modern shape first: three addresses and a ready bit. A legacy device has none of
     * these registers -- they are past the end of its map -- and leaves the ready bit zero,
     * which is how the two are told apart without trusting the version register. */
    uint64_t const desc_at = physical + kDescOffset;
    uint64_t const avail_at = physical + kAvailOffset;
    uint64_t const used_at = physical + kUsedOffset;
    registers.write(kQueueDescLow, static_cast<uint32_t>(desc_at));
    registers.write(kQueueDescHigh, static_cast<uint32_t>(desc_at >> 32));
    registers.write(kQueueDriverLow, static_cast<uint32_t>(avail_at));
    registers.write(kQueueDriverHigh, static_cast<uint32_t>(avail_at >> 32));
    registers.write(kQueueDeviceLow, static_cast<uint32_t>(used_at));
    registers.write(kQueueDeviceHigh, static_cast<uint32_t>(used_at >> 32));
    registers.write(kQueueReady, 1);
    local.desc_back = registers.read(kQueueDescLow);
    local.ready_back = registers.read(kQueueReady);

    if (local.ready_back == 0) {
        /* No ready bit, so the legacy interface. The page frame and the alignment were
         * written above, before the size, for the reason given there. */
        local.legacy = true;
        local.pfn_back = registers.read(kLegacyQueuePfn);
    }

    if (report != nullptr) {
        *report = local;
    }
}

ReadResult read_sector(Registers const &registers, volatile uint8_t *page, uint64_t physical,
                       uint64_t sector, uint8_t *data_out) noexcept
{
    ReadResult result{false, 0, 0, 0, 0, 0, 0};

    /* The request header: a read, of one sector at `sector`. Written *before* it is published,
     * because the device may look as soon as it is told there is something to do. */
    put_word(word_at(page, kRequestOffset + 0), 0, kBlkTypeIn);
    put_word(word_at(page, kRequestOffset + 0), 1, 0);
    volatile uint32_t *sector_words = word_at(page, kRequestOffset + 8);
    put_word(sector_words, 0, static_cast<uint32_t>(sector));
    put_word(sector_words, 1, static_cast<uint32_t>(sector >> 32));
    /* Not zero: whatever is there must be something the device has to overwrite. */
    *byte_at(page, kStatusOffset) = 0xff;

    /* The chain. Three descriptors, each naming a *physical* address the device will read or
     * write: the request (the device reads it), the sector (the device writes it, hence
     * kDescWrite), and the status byte (also written by the device). The last has no NEXT. */
    volatile uint32_t *desc = word_at(page, kDescOffset);
    for (uint32_t i = 0; i < 3; ++i) {
        uint64_t addr = 0;
        uint32_t len = 0;
        uint16_t flags = 0;
        uint16_t next = 0;
        if (i == 0) {
            addr = physical + kRequestOffset;
            len = sizeof(BlkRequest);
            flags = static_cast<uint16_t>(kDescNext);
            next = 1;
        } else if (i == 1) {
            addr = physical + kDataOffset;
            len = kSectorBytes;
            flags = static_cast<uint16_t>(kDescNext | kDescWrite);
            next = 2;
        } else {
            addr = physical + kStatusOffset;
            len = 1;
            flags = kDescWrite;
        }
        put_word(desc, i * 4 + 0, static_cast<uint32_t>(addr));
        put_word(desc, i * 4 + 1, static_cast<uint32_t>(addr >> 32));
        put_word(desc, i * 4 + 2, len);
        put_word(desc, i * 4 + 3,
                 static_cast<uint32_t>(flags) | (static_cast<uint32_t>(next) << 16));
    }

    /* Publish: the head of the chain goes in the available ring, and its index is the last
     * word written. The fence is not decoration -- RISC-V orders stores weakly, and without it
     * the device can see the index before the descriptors it refers to. The in-tree legacy
     * driver brackets its own `avail->idx++` the same way
     * (projects/util_libs/libethdrivers/src/virtio_pci.c:286-289). */
    volatile uint16_t *avail = half_at(page, kAvailOffset);
    avail[0] = 0; /* flags */
    avail[2] = 0; /* ring[0]: the chain starts at descriptor 0 */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    avail[1] = 1; /* idx, last */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    /* The notify carries the queue's index: this is the kick that makes the device look. */
    registers.write(kQueueNotify, 0);

    /* Wait for the device to say it is done. virtio promises progress without an interrupt,
     * so this is a legal way to drive it -- and the bound keeps "it never answered" a report
     * rather than a hang. */
    volatile uint16_t *used = half_at(page, kUsedOffset);
    for (unsigned spin = 0; spin < 200000000 && used[1] == 0; ++spin) {
    }
    /* On a timeout the raw state is the evidence, not a summary: the used ring's own words,
     * and whether the device wrote the status byte at all. A request the device never looked
     * at leaves the sentinel above untouched, and that is a different fault from one it
     * looked at and refused. */
    result.status = *byte_at(page, kStatusOffset);
    result.used_flags = used[0];
    result.used_idx = used[1];
    result.used_bytes = word_at(page, kUsedOffset + 8)[1];
    result.device_status = registers.read(kStatus);
    result.interrupt_status = registers.read(kInterruptStatus);
    if (used[1] == 0) {
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

}  // namespace aegir::virtio
