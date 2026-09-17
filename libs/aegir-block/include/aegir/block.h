/*
 * The protocol a block device serves on its port (specs/services.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * v1, in full. A call carries a method in MR0 and one word in MR1; a reply is
 * one word. Bulk data never crosses the message: each device has a *shared
 * window*, mapped into the driver and into whichever client is calling, and
 * what does not fit in a word -- the identify answer, the sectors a read asked
 * for -- is written there. One window per device is enough because a call is
 * synchronous: requests serialize at the endpoint, so there is exactly one
 * outstanding request per window, and that is structural rather than a lock.
 *
 * The serialization orders the DMAs, not the consumers. Every client's caps
 * name the same physical frames, so the window's content belongs to the most
 * recent call by anyone: a caller must have consumed what it needed before
 * another client of the same port calls again -- and a caller that waits on
 * such a client (a partition manager starting the filesystem for one entry
 * while more entries wait) re-reads what it had when it resumes.
 *
 * The window's size is the driver's to declare (the `window` field of its
 * registry row): a device that benefits from larger transfers declares a
 * larger window, and the spawner provisions without interpreting.
 */

#pragma once

#include <stdint.h>

namespace aegir::block {

/* The methods. */
constexpr uint32_t kMethodIdentify = 1;
constexpr uint32_t kMethodRead = 2;

/** The identify answer, written at offset 0 of the shared window. The device
 *  names *itself* -- "BD0" -- because the public block-device namespace is the
 *  driver's business and nobody else's: the device manager binds instances and
 *  never learns which of them are block devices (specs/services.md). */
struct Identify {
    char name[8];           /* NUL-padded: "BD0" */
    uint64_t sector_count;
    uint32_t sector_size;   /* bytes: 512 here */
    uint32_t window_sectors; /* the most one read may ask for: the window's capacity */
};
static_assert(sizeof(Identify) == 24, "the identify answer is a wire format");

/** A read's MR1: the first sector in the low 48 bits, the sector count minus
 *  one in the high 16 (so count 0 is never representable, and 48 bits of LBA
 *  is 128 PiB of disk). The reply is the number of sectors read -- less than
 *  asked when the device ran out, zero with the window untouched when the
 *  request was not one the device could serve. */
constexpr uint64_t pack_read(uint64_t first_sector, uint32_t count) noexcept
{
    return (first_sector & 0xffffffffffffull) | (static_cast<uint64_t>(count - 1) << 48);
}

constexpr uint64_t read_first(uint64_t packed) noexcept
{
    return packed & 0xffffffffffffull;
}

constexpr uint32_t read_count(uint64_t packed) noexcept
{
    return static_cast<uint32_t>(packed >> 48) + 1;
}

}  // namespace aegir::block
