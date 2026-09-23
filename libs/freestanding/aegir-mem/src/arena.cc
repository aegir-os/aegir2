/*
 * A page-granular bump arena -- implementation. See include/aegir/mem/arena.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/mem/arena.h>

namespace aegir::mem {

namespace {
constexpr uint64_t kPage = 1ull << seL4_PageBits;
constexpr uint64_t kAlignment = 8;
}  // namespace

Arena::Arena(Allocator &allocator, Scratch &scratch, Account &account) noexcept
    : allocator_(allocator), scratch_(scratch), account_(account), current_(nullptr),
      remaining_(0), used_(0), pages_(0)
{
}

void *Arena::allocate(uint64_t bytes) noexcept
{
    uint64_t const wanted = (bytes + kAlignment - 1) & ~(kAlignment - 1);
    if (wanted == 0) {
        return nullptr;
    }
    if (remaining_ < wanted && !start_region(wanted)) {
        return nullptr;
    }

    unsigned char *result = current_;
    current_ += wanted;
    remaining_ -= wanted;
    used_ += wanted;
    /* Frames are recycled memory, so nothing may assume they arrive zeroed.
     * Zeroed here rather than through <string.h>: these boot libraries link no C
     * library symbols at all, and musl's string.h declares its prototypes in a
     * way that collides with GCC's C++ builtins -- the same class of problem as
     * sel4/assert.h's __assert_fail, which libs/aegir-runtime/src/assert.cc
     * handles explicitly. */
    for (uint64_t i = 0; i < wanted; ++i) {
        result[i] = 0;
    }
    return result;
}

bool Arena::start_region(uint64_t bytes) noexcept
{
    /* A request can be larger than one page -- the startup frame for a child is
     * two -- so the region has to be taken as a whole. The window hands addresses
     * out in order, so mapping the pages consecutively *here* is what makes them
     * contiguous; between one request and the next something else may map in the
     * window, which is why the old region is never extended and its tail is
     * abandoned. */
    unsigned const wanted_pages = static_cast<unsigned>((bytes + kPage - 1) / kPage);
    unsigned char *region = nullptr;
    for (unsigned page = 0; page < wanted_pages; ++page) {
        seL4_Error error = seL4_NoError;
        seL4_CPtr frame =
            allocator_.alloc_object(seL4_RISCV_4K_Page, seL4_PageBits, account_, &error);
        if (frame == 0) {
            return false;
        }
        void *mapped = scratch_.map(frame);
        if (mapped == nullptr) {
            return false;
        }
        auto *at = static_cast<unsigned char *>(mapped);
        if (page == 0) {
            region = at;
        } else if (at != region + static_cast<uint64_t>(page) * kPage) {
            /* Not in order: the pages are not contiguous, so one request could
             * not span them, and pretending otherwise writes off the end. */
            return false;
        }
        ++pages_;
    }
    current_ = region;
    remaining_ = static_cast<uint64_t>(wanted_pages) * kPage;
    return true;
}

}  // namespace aegir::mem
