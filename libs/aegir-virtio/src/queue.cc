/*
 * The virtqueue -- implementation. See include/aegir/virtio/queue.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/virtio/queue.h>

namespace aegir::virtio {

namespace {

/* The pages are read and written by both sides, so every access goes through a volatile
 * pointer and the compiler is not free to reorder one past another. Everything here is a
 * small integer at a fixed offset -- a struct assignment through a volatile pointer is not
 * even well formed, and a device does not care about structs anyway. */
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

void Queue::set_up(Registers const &registers, uint32_t index, uint32_t num,
                   QueueReport *report) noexcept
{
    index_ = index;
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
     * driver's side, from a device that is not listening. GuestPageSize is one value for the
     * whole device; a second queue writes it again, unchanged. */
    registers.write(kLegacyGuestPageSize, kPageBytes);

    registers.write(kQueueSel, index);
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
    registers.write(kLegacyQueuePfn, static_cast<uint32_t>(physical_ / kPageBytes));

    /* A queue with no size is a queue the device will not use, so the size is the smaller of
     * what the caller asked for and what the device offers. */
    uint32_t const size = local.num_max < num ? local.num_max : num;
    registers.write(kQueueNum, size);
    local.num_back = registers.read(kQueueNum);

    /* The modern shape first: three addresses and a ready bit. A legacy device has none of
     * these registers -- they are past the end of its map -- and leaves the ready bit zero,
     * which is how the two are told apart without trusting the version register. */
    uint64_t const desc_at = physical_ + kDescOffset;
    uint64_t const avail_at = physical_ + kAvailOffset;
    uint64_t const used_at = physical_ + kUsedOffset;
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

void Queue::use_interrupts(uint64_t notification, uint64_t handler) noexcept
{
    irq_notification_ = static_cast<seL4_CPtr>(notification);
    irq_handler_ = static_cast<seL4_CPtr>(handler);
}

void Queue::publish(Registers const &registers, uint16_t head, ChainBuf const *bufs,
                    uint32_t count) noexcept
{
    /* The chain. Each descriptor names a *physical* address the device will read
     * or write; all but the last continue with NEXT. The buffers are written
     * by the caller *before* this publishes, because the device may look as
     * soon as it is told there is something to do. */
    volatile uint32_t *desc = word_at(page_, kDescOffset);
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t const flags = static_cast<uint16_t>(
            (i + 1 < count ? kDescNext : 0) | (bufs[i].device_writes ? kDescWrite : 0));
        uint16_t const next = static_cast<uint16_t>(i + 1 < count ? head + i + 1 : 0);
        put_word(desc, (head + i) * 4 + 0, static_cast<uint32_t>(bufs[i].physical));
        put_word(desc, (head + i) * 4 + 1, static_cast<uint32_t>(bufs[i].physical >> 32));
        put_word(desc, (head + i) * 4 + 2, bufs[i].bytes);
        put_word(desc, (head + i) * 4 + 3,
                 static_cast<uint32_t>(flags) | (static_cast<uint32_t>(next) << 16));
    }

    /* Publish: the head of the chain goes in the available ring's next slot,
     * and its index is the last word written. The fence is not decoration --
     * RISC-V orders stores weakly, and without it the device can see the index
     * before the descriptors it refers to. The in-tree legacy driver brackets
     * its own `avail->idx++` the same way
     * (projects/util_libs/libethdrivers/src/virtio_pci.c:286-289). */
    volatile uint16_t *avail = half_at(page_, kAvailOffset);
    avail[0] = 0; /* flags */
    avail[2 + next_avail_ % kQueueSize] = head;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    avail[1] = static_cast<uint16_t>(next_avail_ + 1); /* idx, last */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ++next_avail_;
    /* The notify carries the queue's index: this is the kick that makes the device look. */
    registers.write(kQueueNotify, index_);
}

UsedResult Queue::wait_used(Registers const &registers) noexcept
{
    UsedResult result{false, 0, 0, 0, 0, 0, 0};

    /* Wait for the device to say it is done -- for an entry past what the
     * driver has seen: the used index advancing, not being nonzero, which the
     * first request made true for ever. With an interrupt paired, the signal
     * says the device moved: reading the ISR status is what lowers its line
     * (the legacy interface's only acknowledge), the handler's Ack lets the
     * kernel raise the next one, and a signal that was not for the queue -- a
     * config change is the other kind -- just waits again. Without one, virtio
     * promises progress anyway, so polling is legal -- and the bound keeps "it
     * never answered" a report rather than a hang. */
    volatile uint16_t *used = half_at(page_, kUsedOffset);
    if (used[1] == last_used_) {
        if (irq_notification_ != 0) {
            while (used[1] == last_used_) {
                seL4_Word badge = 0;
                seL4_Wait(irq_notification_, &badge);
                static_cast<void>(registers.read(kInterruptStatus));
                seL4_IRQHandler_Ack(irq_handler_);
            }
        } else {
            for (unsigned spin = 0; spin < 200000000 && used[1] == last_used_; ++spin) {
            }
        }
    }
    /* On a timeout the raw state is the evidence, not a summary: the used
     * ring's own words, and the device's status register. */
    result.used_flags = used[0];
    result.used_idx = used[1];
    result.device_status = registers.read(kStatus);
    result.interrupt_status = registers.read(kInterruptStatus);
    if (used[1] == last_used_) {
        return result;
    }
    /* The entry is the one at this driver's own cursor, not the ring's first:
     * the first entry's words are the first request's forever, and a queue
     * that serves many reads the one it is owed. */
    volatile uint32_t *elem =
        word_at(page_, kUsedOffset + 4 + (last_used_ % kQueueSize) * 8);
    result.head = elem[0];
    result.bytes = elem[1];
    ++last_used_;
    result.completed = true;
    return result;
}

}  // namespace aegir::virtio
