/*
 * A run of frames retyped from one untyped -- implementation.
 * See include/aegir/mem/frame_region.h and specs/memory.md.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/mem/frame_region.h>

namespace aegir::mem {

bool FrameRegion::create(Allocator &alloc, Account &account, unsigned piece_bits,
                         unsigned frames, unsigned frame_bits, seL4_CPtr *caps,
                         seL4_Error *error) noexcept
{
    *error = seL4_NoError;
    piece_ = alloc.carve_untyped(piece_bits, account, error, &physical_, &cookie_);
    if (piece_ == 0) {
        return false;
    }
    bits_ = piece_bits;
    for (unsigned f = 0; f < frames; ++f) {
        caps[f] = alloc.carve_page(piece_, account, error, frame_bits);
        if (caps[f] == 0) {
            release(alloc);
            return false;
        }
    }
    frames_ = frames;
    return true;
}

void FrameRegion::release(Allocator &alloc) noexcept
{
    if (piece_ == 0) {
        return;
    }
    /* The allocator revokes the piece before it goes back (specs/memory.md), so
     * the frames and every capability minted from one die with it and the piece
     * is whole again. */
    (void)alloc.free_object(cookie_, bits_);
    piece_ = 0;
    cookie_ = nullptr;
    physical_ = 0;
    bits_ = 0;
    frames_ = 0;
}

}  // namespace aegir::mem