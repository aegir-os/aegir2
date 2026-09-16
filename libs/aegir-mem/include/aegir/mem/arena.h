/*
 * A page-granular bump arena, for the small structures director needs while it
 * is starting up: the parsed manifest, a child's bootstrap block, argument
 * lists. Anything it hands out lives until the arena's owner is done, which is
 * exactly the lifetime boot-time data has.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Memory is charged to the account it is allocated for (specs/authority.md), so
 * a service's boot cost is visible in the same record as everything else it
 * holds. Pages are not returned individually: reclaim is revocation of the
 * account, which is the model, not a gap (specs/authority.md).
 */

#ifndef AEGIR_MEM_ARENA_H
#define AEGIR_MEM_ARENA_H

#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <stdint.h>

namespace aegir::mem {

class Arena {
public:
    Arena(Allocator &allocator, Scratch &scratch, Account &account) noexcept;

    /** `bytes`, zeroed and 8-byte aligned, or nullptr when memory ran out. */
    void *allocate(uint64_t bytes) noexcept;

    uint64_t used() const noexcept { return used_; }
    uint64_t pages() const noexcept { return pages_; }

private:
    bool grow() noexcept;

    Allocator &allocator_;
    Scratch &scratch_;
    Account &account_;
    unsigned char *current_;
    uint64_t remaining_;
    uint64_t used_;
    uint64_t pages_;
};

}  // namespace aegir::mem

#endif  // AEGIR_MEM_ARENA_H
