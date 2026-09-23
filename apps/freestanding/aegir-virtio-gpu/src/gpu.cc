/*
 * The gpu's control commands -- implementation. See src/gpu.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "gpu.h"

namespace aegir::virtio {

namespace {

/* The command and response buffers live in the queue's own memory, past the
 * rings and before the used ring's page -- the same shape as the block
 * driver's request (kUsedOffset is 4096; the rings end at 158). */
constexpr uint32_t kCommandOffset = 512;
constexpr uint32_t kResponseOffset = 640; /* 1 KiB of room: the EDID answer is 1056 */

}  // namespace

GpuResult control(Registers const &registers, Queue &queue, void const *command,
                  uint32_t command_bytes, void *response, uint32_t response_bytes) noexcept
{
    GpuResult result{false, 0xffffffffu, 0};
    volatile uint8_t *page = queue.page();
    uint64_t const physical = queue.physical();

    /* The command is written *before* it is published, because the device may
     * look as soon as it is told there is something to do. The response's
     * type word is poisoned for the same reason the block driver's status
     * byte is: a device that says nothing must be seen. */
    uint8_t const *from = static_cast<uint8_t const *>(command);
    for (uint32_t i = 0; i < command_bytes; ++i) {
        page[kCommandOffset + i] = from[i];
    }
    for (uint32_t i = 0; i < 4; ++i) {
        page[kResponseOffset + i] = 0xff;
    }

    ChainBuf const chain[] = {
        {physical + kCommandOffset, command_bytes, false},
        {physical + kResponseOffset, response_bytes, true},
    };
    queue.publish(registers, 0, chain, 2);

    UsedResult const used = queue.wait_used(registers);
    result.used_bytes = used.bytes;
    if (!used.completed) {
        return result;
    }
    result.completed = true;

    uint8_t *into = static_cast<uint8_t *>(response);
    for (uint32_t i = 0; i < response_bytes; ++i) {
        into[i] = page[kResponseOffset + i];
    }
    result.response_type = static_cast<uint32_t>(into[0]) |
                           (static_cast<uint32_t>(into[1]) << 8) |
                           (static_cast<uint32_t>(into[2]) << 16) |
                           (static_cast<uint32_t>(into[3]) << 24);
    return result;
}

}  // namespace aegir::virtio
