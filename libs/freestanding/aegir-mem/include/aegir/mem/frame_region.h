/*
 * A run of frames retyped from one untyped (specs/memory.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A service that needs memory for a purpose -- a framebuffer, a filesystem's
 * frame, a virtqueue -- needs a run of frames it can map or hand on, and it
 * should not have to retype them itself. The sequence every such service used
 * to hand-roll was
 *
 *     carve_untyped(bits) -> carve_page(untyped, bits) x N -> map -> revoke -> free
 *
 * and each copy had to get the capability lifecycle right: a piece is handed
 * back with `Revoke`, never `Delete`; the capability it was carved from is the
 * allocator's; and the kernel's free index lives *in the capability*, so a
 * copied capability and the original disagree about how much is left. The
 * failure when one copy got it wrong landed on an application that never
 * touched any of it.
 *
 * `FrameRegion` is that sequence in one place. The caller names how many frames
 * it wants and of what size and provides the capability array (a capacity, so
 * the library holds no table); the region carves the untyped, retypes the
 * frames, and on release gives the piece back -- whole, because the allocator
 * revokes it. The service never sees an untyped capability.
 */

#ifndef AEGIR_MEM_FRAME_REGION_H
#define AEGIR_MEM_FRAME_REGION_H

#include <sel4/sel4.h>
#include <stdint.h>

#include <aegir/mem/allocator.h>

namespace aegir::mem {

class FrameRegion {
public:
    /**
     * Carve a piece of `piece_bits` and retype `frames` frames of `frame_bits`
     * from it, one capability per frame into `caps` (the caller's array).
     * Returns false and writes `*error` when there is no memory or no slot, and
     * leaves nothing behind -- a partly built region is released.
     */
    bool create(Allocator &alloc, Account &account, unsigned piece_bits, unsigned frames,
                unsigned frame_bits, seL4_CPtr *caps, seL4_Error *error) noexcept;

    /** The piece's physical base, or 0 when the giver did not say: the one thing
     *  a device has to be told (allocator.h's `carve_untyped`). */
    uint64_t physical() const noexcept { return physical_; }

    /** How many frames the region holds. */
    unsigned frames() const noexcept { return frames_; }

    /** Whether a region is held. */
    bool held() const noexcept { return piece_ != 0; }

    /**
     * Give the piece back. The allocator revokes it, so every frame and every
     * capability minted from one dies with it, and the piece returns to its free
     * list whole. `caps` are dead afterwards; the region is empty and may be
     * created again.
     */
    void release(Allocator &alloc) noexcept;

private:
    seL4_CPtr piece_ = 0;
    void *cookie_ = nullptr;
    uint64_t physical_ = 0;
    unsigned bits_ = 0;
    unsigned frames_ = 0;
};

}  // namespace aegir::mem

#endif  // AEGIR_MEM_FRAME_REGION_H