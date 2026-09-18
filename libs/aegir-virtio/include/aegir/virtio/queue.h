/*
 * The virtqueue: the one structure a virtio device and its driver share.
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
 * Which is why a driver needs to be told where its memory is: the device reads these
 * structures by physical address, and nothing a service can ask the kernel would say where
 * its own page is. Everything here is offsets from the start of the queue's pages, laid
 * out so the descriptor table is first, the available ring follows it, and the *used* ring
 * starts at the next QueueAlign boundary after that (kUsedOffset below).
 *
 * The queue can be driven two ways. By interrupt, when the spawner paired one
 * with the device (`use_interrupts`): the kick is followed by a wait on the
 * notification, and the used ring has advanced when the wait returns. By
 * polling, when it was not: virtio guarantees that a driver which publishes a
 * request makes progress without an interrupt, so spinning on the used index
 * is legal -- and the bound on the loop turns "it never answered" into a
 * report rather than a hang. Interrupts are the default on a machine that
 * has them; polling is the fallback a driver without a handler cap takes.
 *
 * The state is an object, not a file's globals, because the third driver has
 * two queues (virtio-input's eventq and statusq): what was true of "the queue"
 * when only the block driver existed is true of each queue now.
 */

#pragma once

#include <aegir/virtio/mmio.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::virtio {

/* One queue, eight entries. A power of two because the rings wrap by masking, and smaller
 * than the devices' QueueNumMax (1024 here) because nothing needs more. */
constexpr uint32_t kQueueSize = 8;

/* The pages' layout, in bytes. Three rules from the header the structures come from: the
 * descriptor table is first, the available ring follows it at 16 bytes per entry, and the
 * *used* ring starts at the next QueueAlign boundary after that -- which is what bounded
 * alignment means, and why the legacy path declares 4 rather than a page. Each ring ends with
 * the event field the *other* side reads: `used_event` after available, `avail_event` after
 * used. */
constexpr uint32_t kDescOffset = 0;
constexpr uint32_t kAvailOffset = kDescOffset + kQueueSize * 16;         /* 128 */
/* The used ring is a *page* away, not packed after the available ring: `vring_init` puts it
 * at `align(&avail->ring[num] + sizeof(uint16_t), align)` and the alignment is 4096
 * (projects/util_libs/libvirtio/include/virtio/virtio_pci.h, VIRTIO_PCI_VRING_ALIGN -- "the
 * alignment to use between consumer and producer parts of vring"). That is the whole reason
 * this queue needs two pages: 150 rounded up to 4096 is a second page, and a device writing
 * its used entry at base+4096 has been writing outside the page the driver polls. */
constexpr uint32_t kUsedOffset = 4096;
constexpr uint32_t kQueueBytes = 8192;
constexpr uint32_t kPageBytes = 4096;

/* The descriptor flags (virtio_ring.h: VRING_DESC_F_*). */
constexpr uint16_t kDescNext = 1;   /* the next descriptor continues this chain */
constexpr uint16_t kDescWrite = 2;  /* the *device* writes this buffer, not the driver */

/** What the device says about the queue it was given, read back after setup. A device that
 *  kept nothing answers zero here, which is worth knowing before blaming the ring. */
struct QueueReport {
    uint32_t num_back;
    uint32_t ready_back;
    uint32_t desc_back;
    uint32_t num_max;
    bool legacy;
    uint32_t pfn_back;
    uint32_t pfn_before; /* what QueuePFN held before: nonzero means already in use */
};

/** One buffer of a chain to publish: where it is *physically*, how long, and
 *  whether the device writes it. */
struct ChainBuf {
    uint64_t physical;
    uint32_t bytes;
    bool device_writes;
};

/** What one used entry said. On a timeout -- `completed` false -- the raw
 *  state is the evidence rather than a summary: the used ring's own words, the
 *  device's status register (a device that could not map the rings raises
 *  VIRTIO_CONFIG_S_NEEDS_RESET, 0x40, there), and the interrupt status. */
struct UsedResult {
    bool completed;
    uint32_t head;  /* the published chain's first descriptor */
    uint32_t bytes; /* what the device wrote across the chain */
    uint32_t used_flags;
    uint32_t used_idx;
    uint32_t device_status;
    uint32_t interrupt_status;
};

/** One virtqueue: the pages it lives in, its index on the device, the ring
 *  cursors, and the interrupt it was paired with. `place` before `set_up`,
 *  `set_up` before DRIVER_OK (virtio 1.x, 2.1.1 step 8: the status bit is the
 *  driver saying everything is ready, and a device told "go" before its queue
 *  exists may ignore the queue entirely). */
class Queue {
public:
    /** The pages the queue lives in: `page` as the driver maps it, `physical`
     *  the machine address the device is told. */
    void place(volatile uint8_t *page, uint64_t physical) noexcept
    {
        page_ = page;
        physical_ = physical;
    }

    volatile uint8_t *page() const noexcept { return page_; }
    uint64_t physical() const noexcept { return physical_; }

    /** Set the queue up: tell the device where the pages are *physically*.
     *  Handles both layouts: the modern registers are written first, and a
     *  device that answers no ready bit is driven legacy. `index` is the
     *  device's queue number -- 0 for a one-queue device. */
    void set_up(Registers const &registers, uint32_t index, uint32_t num,
                QueueReport *report) noexcept;

    /** Pair an interrupt with the queue: a notification to wait on after the
     *  kick, and the IRQ handler to ack after each signal. Without it every
     *  wait is completed by polling. Slots travel as uint64_t, the way the
     *  bootstrap block hands them out. */
    void use_interrupts(uint64_t notification, uint64_t handler) noexcept;

    /** Publish a chain of `count` buffers starting at descriptor `head`, and
     *  kick. The descriptors a chain occupies are the caller's discipline:
     *  a one-request-at-a-time driver reuses descriptor 0; a queue with many
     *  buffers outstanding -- an input device's eventq -- gives each buffer
     *  its own. */
    void publish(Registers const &registers, uint16_t head, ChainBuf const *bufs,
                 uint32_t count) noexcept;

    /** Wait for one used entry past what this queue has seen, and consume it.
     *  `completed` is false only on a poll timeout. */
    UsedResult wait_used(Registers const &registers) noexcept;

private:
    volatile uint8_t *page_ = nullptr;
    uint64_t physical_ = 0;
    uint32_t index_ = 0;
    /* The driver-side cursors of the two rings. A ring's index only ever
     * advances, and the device only ever appends: which available slot the
     * next chain takes, and which used entry the driver has seen, are what a
     * second request needs that a first one did not -- the queue was
     * single-shot until the block port made it a service. */
    uint16_t next_avail_ = 0;
    uint16_t last_used_ = 0;
    /* The interrupt the queue was paired with, when it was: a notification to
     * wait on and the handler to ack, both zero while the queue is driven by
     * polling. */
    seL4_CPtr irq_notification_ = 0;
    seL4_CPtr irq_handler_ = 0;
};

}  // namespace aegir::virtio
