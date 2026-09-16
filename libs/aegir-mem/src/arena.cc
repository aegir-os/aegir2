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
    uint64_t wanted = (bytes + kAlignment - 1) & ~(kAlignment - 1);
    if (wanted == 0) {
        return nullptr;
    }
    if (wanted > remaining_ && !grow()) {
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

bool Arena::grow() noexcept
{
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
    current_ = static_cast<unsigned char *>(mapped);
    remaining_ = kPage;
    ++pages_;
    return true;
}

}  // namespace aegir::mem
