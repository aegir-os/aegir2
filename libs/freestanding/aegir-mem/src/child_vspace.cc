/*
 * A child's address space -- implementation. See include/aegir/mem/child_vspace.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/mem/child_vspace.h>

/* seL4_MappingFailedLookupLevel and SEL4_MAPPING_LOOKUP_NO_PT: the arch header
 * that says which level a failed mapping wanted
 * (kernel/libsel4/sel4_arch_include/riscv64/sel4/sel4_arch/mapping.h:15-21). */
#include <sel4/sel4_arch/mapping.h>

namespace aegir::mem {

namespace {
constexpr uint64_t kPage = 1ull << seL4_PageBits;
}  // namespace

ChildVSpace::ChildVSpace(Allocator &allocator, Scratch &scratch) noexcept
    : allocator_(allocator), scratch_(scratch), root_(0), mapped_pages_(0), mapped_bytes_(0)
{
}

bool ChildVSpace::create(seL4_CPtr pool, Account &account) noexcept
{
    seL4_Error error = seL4_NoError;
    /* The root of a RISC-V VSpace is a page table, and the kernel is content to
     * be handed it as the `vspace` argument of every map below. */
    root_ = allocator_.alloc_object(seL4_RISCV_PageTableObject, seL4_PageTableBits, account, &error);
    if (root_ == 0) {
        return false;
    }
    /* A VSpace root is unusable until it has an address space id, and a TCB
     * cannot be configured with it (the kernel refuses with "not assigned to an
     * ASID pool",
     * out/aegir/libsel4/include/interfaces/sel4_client.h:884). The pool is the
     * caller's -- the spawner's -- and this is the step that makes "an address
     * space is built from a pool" true in specs/authority.md. */
    return seL4_RISCV_ASIDPool_Assign(pool, root_) == seL4_NoError;
}

bool ChildVSpace::map_page(uintptr_t address, seL4_CPtr frame, bool writable,
                           Account &account, seL4_Error *error_out,
                           seL4_Word size_bits) noexcept
{
    if (error_out != nullptr) {
        *error_out = seL4_NoError;
    }
    if (root_ == 0 || frame == 0) {
        if (error_out != nullptr) {
            *error_out = seL4_InvalidCapability;
        }
        return false;
    }
    seL4_CapRights_t const rights = writable ? seL4_AllRights : seL4_CanRead;
    seL4_Error error = seL4_RISCV_Page_Map(frame, root_, address, rights,
                                           seL4_RISCV_Default_VMAttributes);

    /* The page tables above this page may not exist yet. Which one is missing is
     * in the error, but there is no need to ask: on RISC-V the object to create
     * is a page table at whatever level failed -- the arch helper ignores the
     * level entirely and returns seL4_RISCV_PageTableObject
     * (projects/seL4_libs/libsel4vspace/src/arch/riscv/mapping.c:18-27) -- and a
     * fresh VSpace needs both the intermediate table and the one above it, so
     * looping here creates the tree the mapping needs, one level per pass. */
    unsigned attempts = 0;
    while (error == seL4_FailedLookup && attempts < 8) {
        ++attempts;
        seL4_Error created = seL4_NoError;
        void *cookie = nullptr;
        seL4_CPtr table = allocator_.alloc_object(seL4_RISCV_PageTableObject, seL4_PageTableBits,
                                                  account, &created, &cookie);
        if (table == 0) {
            return false;
        }
        /* Mapped at the address being mapped, not at its region's base: the
         * kernel places the table in the slot for that address. */
        seL4_Error mapped =
            seL4_RISCV_PageTable_Map(table, root_, address, seL4_RISCV_Default_VMAttributes);
        if (mapped == seL4_DeleteFirst) {
            /* A table already sits at the slot this address's walk reaches (an
             * address that is a table boundary): ours is redundant, so give its
             * memory back and retry (libsel4utils mapping.c:67-70). */
            allocator_.free_object(cookie, seL4_PageTableBits);
        } else if (mapped != seL4_NoError) {
            return false;
        }
        error = seL4_RISCV_Page_Map(frame, root_, address, rights, seL4_RISCV_Default_VMAttributes);
    }
    if (error != seL4_NoError) {
        if (error_out != nullptr) {
            *error_out = error;
        }
        return false;
    }
    ++mapped_pages_;
    mapped_bytes_ += 1ull << size_bits;
    return true;
}

bool ChildVSpace::populate(uintptr_t address, unsigned pages, void const *source, uint64_t bytes,
                           uint64_t leading, bool writable, Account &account,
                           seL4_CPtr *first_frame, char const **why,
                           seL4_CPtr *frames_out) noexcept
{
    if (why != nullptr) {
        *why = "";
    }
    if (leading >= kPage || bytes + leading > static_cast<uint64_t>(pages) * kPage) {
        if (why != nullptr) {
            *why = "the bytes do not fit the pages";
        }
        return false;
    }

    auto const *input = static_cast<unsigned char const *>(source);
    uint64_t remaining = bytes;
    uint64_t skip = leading;
    for (unsigned page = 0; page < pages; ++page) {
        seL4_Error error = seL4_NoError;
        seL4_CPtr frame =
            allocator_.alloc_object(seL4_RISCV_4K_Page, seL4_PageBits, account, &error);
        if (frame == 0) {
            if (why != nullptr) {
                *why = "no memory for a frame";
            }
            return false;
        }
        if (page == 0 && first_frame != nullptr) {
            *first_frame = frame;
        }
        if (frames_out != nullptr) {
            frames_out[page] = frame;
        }

        /* Fill it first, through our own window: once it is mapped into the
         * child there is no way for us to reach it. */
        void *window = scratch_.map(frame);
        if (window == nullptr) {
            if (why != nullptr) {
                *why = "the window the frame is filled through is full";
            }
            return false;
        }
        auto *destination = static_cast<unsigned char *>(window);
        for (uint64_t i = 0; i < kPage; ++i) {
            destination[i] = 0;
        }
        uint64_t const room = kPage - skip;
        uint64_t const chunk = remaining < room ? remaining : room;
        for (uint64_t i = 0; i < chunk; ++i) {
            destination[skip + i] = input[i];
        }
        skip = 0;
        scratch_.unmap(frame);

        if (!map_page(address + static_cast<uintptr_t>(page) * kPage, frame, writable, account)) {
            if (why != nullptr) {
                *why = "a page could not be mapped into the child";
            }
            return false;
        }
        input += chunk;
        remaining -= chunk;
    }
    return true;
}

bool ChildVSpace::populate_fetched(uintptr_t address, unsigned pages, uint64_t file_offset,
                                   uint64_t bytes, uint64_t leading, bool writable,
                                   Account &account, ByteSource fetch, void *fetch_context,
                                   seL4_CPtr *first_frame, char const **why,
                                   seL4_CPtr *frames_out) noexcept
{
    if (why != nullptr) {
        *why = "";
    }
    if (fetch == nullptr || leading >= kPage ||
        bytes + leading > static_cast<uint64_t>(pages) * kPage) {
        if (why != nullptr) {
            *why = "the bytes do not fit the pages";
        }
        return false;
    }

    uint64_t remaining = bytes;
    uint64_t read_at = file_offset;
    uint64_t skip = leading;
    for (unsigned page = 0; page < pages; ++page) {
        seL4_Error error = seL4_NoError;
        seL4_CPtr frame =
            allocator_.alloc_object(seL4_RISCV_4K_Page, seL4_PageBits, account, &error);
        if (frame == 0) {
            if (why != nullptr) {
                *why = "no memory for a frame";
            }
            return false;
        }
        if (page == 0 && first_frame != nullptr) {
            *first_frame = frame;
        }
        if (frames_out != nullptr) {
            frames_out[page] = frame;
        }

        /* Fill it first, through our own window, exactly as `populate` does --
         * but the bytes come from the source rather than a buffer we hold. */
        void *window = scratch_.map(frame);
        if (window == nullptr) {
            if (why != nullptr) {
                *why = "the window the frame is filled through is full";
            }
            return false;
        }
        auto *destination = static_cast<unsigned char *>(window);
        for (uint64_t i = 0; i < kPage; ++i) {
            destination[i] = 0;
        }
        uint64_t const room = kPage - skip;
        uint64_t const chunk = remaining < room ? remaining : room;
        if (chunk != 0 &&
            !fetch(fetch_context, read_at, chunk, destination + skip)) {
            if (why != nullptr) {
                *why = "a segment's bytes could not be read";
            }
            return false;
        }
        skip = 0;
        scratch_.unmap(frame);

        if (!map_page(address + static_cast<uintptr_t>(page) * kPage, frame, writable, account)) {
            if (why != nullptr) {
                *why = "a page could not be mapped into the child";
            }
            return false;
        }
        read_at += chunk;
        remaining -= chunk;
    }
    return true;
}

bool ChildVSpace::populate_frames(uintptr_t address, unsigned pages, uint64_t file_offset,
                                  uint64_t bytes, uint64_t leading, bool writable,
                                  Account &account, FrameSource fill, void *fill_context,
                                  seL4_CPtr *first_frame, char const **why,
                                  seL4_CPtr *frames_out) noexcept
{
    if (why != nullptr) {
        *why = "";
    }
    if (fill == nullptr || leading >= kPage ||
        bytes + leading > static_cast<uint64_t>(pages) * kPage) {
        if (why != nullptr) {
            *why = "the bytes do not fit the pages";
        }
        return false;
    }

    /* The 4 KiB pages [first, last), read from the file starting at `read_at`
     * with `skip` bytes of the first page already accounted for. Retype the
     * largest power-of-two run the remaining pages allow in one kernel call
     * (alloc_pages_run): a run whose whole piece the allocator cannot find
     * halves until one fits, so a fragmentary allocator just gives smaller runs
     * rather than failing. */
    auto populate_4k = [&](unsigned first, unsigned last, uint64_t read_at, uint64_t skip,
                           uint64_t remaining) -> bool {
        unsigned page = first;
        while (page < last) {
            unsigned run = 1;
            while (run * 2 <= last - page) {
                run *= 2;
            }
            seL4_Error error = seL4_NoError;
            seL4_CPtr run_base = 0;
            while (run > 0) {
                run_base = allocator_.alloc_pages_run(run, account, &error);
                if (run_base != 0) {
                    break;
                }
                run >>= 1;
            }
            if (run_base == 0) {
                if (why != nullptr) {
                    *why = "no memory for a frame";
                }
                return false;
            }
            for (unsigned k = 0; k < run; ++k, ++page) {
                seL4_CPtr const frame = run_base + k;
                if (page == 0 && first_frame != nullptr) {
                    *first_frame = frame;
                }
                if (frames_out != nullptr) {
                    frames_out[page] = frame;
                }

                uint64_t const room = kPage - skip;
                uint64_t const chunk = remaining < room ? remaining : room;
                /* A page the source fills whole needs nothing more. A boundary
                 * page -- the first, only partly covered because a segment need
                 * not start on a page boundary, or the last -- has bytes the
                 * segment does not reach, and those must be zero. A retyped
                 * frame cannot be relied on to be zero (the BFS zeroes its own
                 * handle page for exactly this reason, aegir-fs-bfs/src/
                 * main.cc), so the gap is zeroed through our own window first,
                 * the same map-write-unmap rhythm populate keeps. The source
                 * then writes the segment's bytes at `skip`. */
                if (skip != 0 || chunk < room) {
                    void *window = scratch_.map(frame);
                    if (window == nullptr) {
                        if (why != nullptr) {
                            *why = "the window the frame is zeroed through is full";
                        }
                        return false;
                    }
                    auto *zeroed = static_cast<unsigned char *>(window);
                    for (uint64_t i = 0; i < kPage; ++i) {
                        zeroed[i] = 0;
                    }
                    scratch_.unmap(frame);
                }
                if (chunk != 0 &&
                    !fill(fill_context, read_at, chunk, skip, frame, seL4_PageBits)) {
                    if (why != nullptr) {
                        *why = "a segment's bytes could not be read";
                    }
                    return false;
                }
                skip = 0;

                if (!map_page(address + static_cast<uintptr_t>(page) * kPage, frame, writable,
                              account)) {
                    if (why != nullptr) {
                        *why = "a page could not be mapped into the child";
                    }
                    return false;
                }
                read_at += chunk;
                remaining -= chunk;
            }
        }
        return true;
    };

    /* The 2 MiB-aligned blocks every byte of which is the segment's own loaded
     * bytes become mega pages: one mapping where 512 4 KiB pages were, and one
     * filesystem call where 512 were. A block the segment fills only partly --
     * its head, its tail, or a .bss gap -- stays 4 KiB, because a 2 MiB frame
     * cannot be zeroed through the small scratch window. The first page is kept
     * 4 KiB so `first_frame` stays a page the IPC buffer can be handed. */
    constexpr uint64_t kMega = 1ull << seL4_LargePageBits;
    uintptr_t const file_low = address + leading;
    uintptr_t const file_high = file_low + bytes;
    uintptr_t mega_low = (file_low + kMega - 1) & ~(kMega - 1);
    uintptr_t const mega_high = file_high & ~(kMega - 1);
    if (mega_low < address + kPage) {
        mega_low = address + kPage;
    }
    if (mega_low < mega_high) {
        unsigned const head_end = static_cast<unsigned>((mega_low - address) / kPage);
        unsigned const tail_first = static_cast<unsigned>((mega_high - address) / kPage);
        if (!populate_4k(0, head_end, file_offset, leading, bytes)) {
            return false;
        }
        for (uintptr_t block = mega_low; block < mega_high; block += kMega) {
            seL4_Error error = seL4_NoError;
            seL4_CPtr const frame = allocator_.alloc_object(seL4_RISCV_Mega_Page,
                                                            seL4_LargePageBits, account, &error);
            if (frame == 0) {
                if (why != nullptr) {
                    *why = "no memory for a mega page";
                }
                return false;
            }
            if (!fill(fill_context, file_offset + (block - file_low), kMega, 0, frame,
                      seL4_LargePageBits)) {
                if (why != nullptr) {
                    *why = "a segment's bytes could not be read";
                }
                return false;
            }
            if (!map_page(block, frame, writable, account, nullptr, seL4_LargePageBits)) {
                if (why != nullptr) {
                    *why = "a mega page could not be mapped into the child";
                }
                return false;
            }
        }
        uintptr_t const tail_at = address + static_cast<uintptr_t>(tail_first) * kPage;
        return populate_4k(tail_first, pages, file_offset + (tail_at - file_low), 0,
                           file_high - tail_at);
    }
    return populate_4k(0, pages, file_offset, leading, bytes);
}

}  // namespace aegir::mem
