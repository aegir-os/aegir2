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

/** Where the bytes that fill a frame come from when they are not held whole:
 *  the source fills `destination` with `length` bytes read at `offset`,
 *  returning false when it cannot. A program image read straight into the
 *  child's frames uses this, so the spawner keeps no copy of the image
 *  (specs/director.md's spawn path). */
using ByteSource = bool (*)(void *context, uint64_t offset, uint64_t length,
                            void *destination);

/** The same, for a source that writes into a frame of ours rather than into a
 *  buffer: the source fills `frame` -- a page capability -- with `length` bytes
 *  read at `offset`, starting `frame_offset` bytes in, returning false when it
 *  cannot. `frame_bits` is the frame's size, and the source must map it so:
 *  a 4 KiB page (kFrameBitsMin) or a 2 MiB mega page (kFrameBitsMax), which a
 *  loader uses for a large segment's aligned bulk. A filesystem that maps a
 *  caller's frame (aegir/volume.h's read-frame) is written this way, so the
 *  spawner neither maps the frame nor copies it (specs/vfs.md's scaling path). */
using FrameSource = bool (*)(void *context, uint64_t offset, uint64_t length,
                             uint64_t frame_offset, seL4_CPtr frame,
                             uint32_t frame_bits);

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

    /**
     * `populate`, but the bytes are pulled from `fetch` instead of a buffer the
     * caller holds whole: each frame is filled by calling
     * `fetch(fetch_context, file_offset + consumed, chunk, window + skip)`. This
     * is how a program image is read straight into the child's frames -- the
     * file is the source, and no whole-image copy is kept
     * (specs/director.md's spawn path). `file_offset` is where the segment's
     * bytes start in the source.
     */
    bool populate_fetched(uintptr_t address, unsigned pages, uint64_t file_offset,
                          uint64_t bytes, uint64_t leading, bool writable, Account &account,
                          ByteSource fetch, void *fetch_context,
                          seL4_CPtr *first_frame = nullptr, char const **why = nullptr,
                          seL4_CPtr *frames_out = nullptr) noexcept;

    /**
     * `populate_fetched`, but the source writes the frame *itself*: it fills
     * `frame` -- a page capability of ours -- with `length` bytes read at
     * `offset`, starting `frame_offset` bytes in. This is the bulk path
     * (specs/vfs.md's read-frame): a filesystem that maps a caller's frame puts
     * the segment's bytes straight into the frame the child will hold, one call
     * per page instead of the several an inline read needs, and the bytes never
     * cross a message. A page the source fills whole is not touched here; a
     * boundary page's gap is zeroed through our own window before the fill (a
     * retyped frame is not relied on to be zero).
     */
    bool populate_frames(uintptr_t address, unsigned pages, uint64_t file_offset,
                         uint64_t bytes, uint64_t leading, bool writable, Account &account,
                         FrameSource fill, void *fill_context,
                         seL4_CPtr *first_frame = nullptr, char const **why = nullptr,
                         seL4_CPtr *frames_out = nullptr) noexcept;

    /** Map `frame` at `address`, creating the page tables above it if they are
     *  missing. `error`, when given, is why it did not work -- the kernel's own
     *  answer, because "it did not work" has several of them and they mean
     *  different things (kernel/manual/parts/vspace.tex).
     *  `size_bits` is the frame's size -- a mega page (seL4_LargePageBits) maps
     *  at the level above a 4 KiB one, which the table-provisioning loop gets
     *  right on its own: the kernel says FailedLookup one level fewer times.
     *  The address must be aligned to the frame's size. */
    bool map_page(uintptr_t address, seL4_CPtr frame, bool writable, Account &account,
                  seL4_Error *error = nullptr, seL4_Word size_bits = seL4_PageBits) noexcept;

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
