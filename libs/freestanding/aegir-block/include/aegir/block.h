/*
 * The protocol a block device serves on its port (specs/services.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * v1, in full. A call carries a method in MR0 and words after it -- one for
 * a read or a write, three for a clamp, two for a discard, none for caps; a
 * reply is one word, or two for caps. Bulk data never
 * crosses the message: each *client* has a window of its own, mapped into the
 * driver and into that client, and what does not fit in a word -- the identify
 * answer, the sectors a read asked for or a write carries -- is written there.
 *
 * One window per client, not per device, is what makes the DMA safe. The
 * endpoint serializes requests, but it cannot stop one client's window from
 * being written while that client is preempted between its call and its
 * consumption; if every client mapped the same frames, the window's content
 * would belong to the most recent call by *anyone* and a caller would resume
 * to another's data. So the frames a client reads from are its own. The
 * window is placed by whoever starts the client -- the partition manager
 * carves a window per filesystem and mints its frames into the child, the
 * device manager carves its own -- and its physical base is recorded with the
 * driver alongside the client's clamp (kMethodClamp), because a driver points
 * a device's DMA at a physical address and has no other way to learn which
 * window belongs to which caller. The badge-0 caller, the device's manager,
 * reads through the window it was itself started with.
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
/* A range grant, enforced: words are {badge, first sector, sector count,
 * window physical}. Only the badge-0 caller may record one -- the device's
 * manager, whose cap is the unbadged one -- once per badge, and before the
 * child that will hold the badge exists. The window physical is the client's
 * own window (see above), which the manager carved when it started the child;
 * it is how the driver learns where the caller's data goes. The reply is 1
 * for recorded, 0 for refused (a badge already clamped, a range off the
 * device, a caller that is not the manager). Reads and writes then clamp by
 * badge and land in that badge's window: badge 0 is the whole device through
 * the manager's own window, any other badge reads only inside its recorded
 * range, and an unrecorded badge reads nothing (specs/services.md). */
constexpr uint32_t kMethodClamp = 3;
/* The write: the same packed word as a read, the same clamp by badge, the
 * reply the number of sectors written. The sectors cross through the window
 * exactly as a read's do, in the other direction: the caller fills the
 * window, then calls. */
constexpr uint32_t kMethodWrite = 4;
/* What the device can do beyond sectors: no words, an answer of two words --
 * flags then max_discard_sectors, the BlockCaps fields below. The answer
 * crosses in the reply rather than the window, because a window belongs to
 * whoever started the caller and the driver maps only its own; a filesystem
 * asking caps on mount has no window the driver could write. */
constexpr uint32_t kMethodCaps = 5;
/* The trim: words are {first sector, sector count}. The same clamp by badge
 * that read and write take, and no data at all -- a discard is a hint that a
 * range holds nothing worth keeping, and the device is free to treat it as a
 * no-op. The reply is the number of sectors discarded, zero when the device
 * does not support discard or the range was refused; a filesystem that reads
 * zero stops asking (specs/bfs.md). */
constexpr uint32_t kMethodDiscard = 6;

/** The capabilities answer: bit 0 of `flags` is "discard is supported", and
 *  `max_discard_sectors` is the largest range one discard request may name,
 *  in 512-byte sectors. It travels as the two words of a kMethodCaps reply,
 *  in that order. */
struct BlockCaps {
    uint32_t flags;
    uint32_t max_discard_sectors;
};

constexpr uint32_t kCapsDiscard = 1u;

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
