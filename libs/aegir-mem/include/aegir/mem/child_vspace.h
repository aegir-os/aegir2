/*
 * The address space of a process being built.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A child's address space is a fresh one: nothing is mapped in it, so every page
 * needs the page tables above it created as well. The kernel says which level is
 * missing when a mapping fails, so the loop here is the upstream idiom rather
 * than our own bookkeeping of what exists
 * (projects/seL4_libs/libsel4utils/src/mapping.c:47-78):
 *
 *     map the page -> seL4_FailedLookup -> ask which level ->
 *     create that object, map it, retry
 *
 * Pages are also filled *through us*, because we cannot write through someone
 * else's page tables: each frame is mapped transiently into our own window
 * (aegir/mem/vspace.h) and written there. That is why populating a region is one
 * operation rather than "allocate, then fill".
 */

#ifndef AEGIR_MEM_CHILD_VSPACE_H
#define AEGIR_MEM_CHILD_VSPACE_H

#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <stdint.h>

namespace aegir::mem {

class ChildVSpace {
public:
    ChildVSpace(Allocator &allocator, Scratch &scratch) noexcept;

    /** Create the root page table and give it an address space id from `pool`.
     *  The pool is the spawner's -- director's own initial pool, or the one a
     *  service was delegated (specs/authority.md) -- and nothing is mapped until
     *  `populate`. */
    bool create(seL4_CPtr pool, Account &account) noexcept;

    /**
     * Retype `pages` frames, map them at `address` (read/write when `writable`,
     * otherwise read-only), and copy `bytes` of `source` into them starting
     * `leading` bytes into the first page, zero-filling everything else.
     *
     * `leading` exists because an ELF segment does not have to start on a page
     * boundary -- aegir-hello's data segment starts at 0x13da0 -- and the loader's
     * job is to map the page it *is* in and put the bytes at the right offset.
     *
     * `first_frame`, when given, receives the first page's capability: the IPC
     *  buffer needs exactly that to be handed to a TCB.
     *
     * `frames_out`, when given, receives every frame -- room for `pages` of
     *  them: a copy made once and mapped into every later child is how the
     *  initrd reaches the services that spawn (specs/services.md), and the
     *  caps are what the sharing maps.
     *
     * `why`, when given, is what failed -- "it did not work" has three causes
     * here (the argument check, the frame, the map) and the spawner's report is
     * only useful if it says which.
     */
    bool populate(uintptr_t address, unsigned pages, void const *source, uint64_t bytes,
                  uint64_t leading, bool writable, Account &account,
                  seL4_CPtr *first_frame = nullptr, char const **why = nullptr,
                  seL4_CPtr *frames_out = nullptr) noexcept;

    /** Map one page, creating whatever page tables the kernel says are missing. */
    /** Map `frame` at `address`, creating the page tables above it if they are
     *  missing. `error`, when given, is why it did not work -- the kernel's own
     *  answer, because "it did not work" has several of them and they mean
     *  different things (kernel/manual/parts/vspace.tex). */
    bool map_page(uintptr_t address, seL4_CPtr frame, bool writable, Account &account,
                  seL4_Error *error = nullptr) noexcept;

    seL4_CPtr root() const noexcept { return root_; }
    unsigned mapped_pages() const noexcept { return mapped_pages_; }
    uint64_t mapped_bytes() const noexcept { return mapped_bytes_; }

private:
    Allocator &allocator_;
    Scratch &scratch_;
    seL4_CPtr root_;
    unsigned mapped_pages_;
    uint64_t mapped_bytes_;
};

}  // namespace aegir::mem

#endif  // AEGIR_MEM_CHILD_VSPACE_H
