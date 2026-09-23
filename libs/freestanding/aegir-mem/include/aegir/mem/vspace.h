/*
 * Our own address space, for mappings Aegir needs.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The spawner has to fill a frame through a mapping *it* can write through
 * before the frame is handed to a child, and the child's address space is a
 * separate object (specs/director.md). This is the "we can write through it"
 * half: a scratch window in our own VSpace.
 *
 * The window is derived, not chosen. It starts above everything the kernel told
 * us is ours -- the image, the IPC buffer, the bootinfo frame and the extended
 * bootinfo pages -- and it stops at the end of the large-page region those live
 * in, because that region's page tables already exist. Mapping past it needs
 * page-table creation, which is the same code a child VSpace needs, so it
 * arrives with that rather than being faked here.
 */

#ifndef AEGIR_MEM_VSPACE_H
#define AEGIR_MEM_VSPACE_H

#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::mem {

class Allocator;

class Scratch {
public:
    explicit Scratch(seL4_BootInfo *bootinfo) noexcept;

    /** Work out the window from the bootinfo. `tables`, when given, lets the
     *  window grow: past the first large page nothing is promised to be
     *  mapped, so reaching the window's end extends it a large page at a time
     *  and missing page tables are allocated as the kernel asks for them (the
     *  same FailedLookup idiom as a service's adopted window). A boot set
     *  that grows past one large page of scratch -- another service's image,
     *  stack and arena pages -- is what makes this the root task's problem
     *  and not only a service's. */
    bool initialise(Allocator *tables) noexcept;

    /** Adopt a window in our own address space when we are a *service*: there is
     *  no bootinfo to derive one from, so the spawner that owns our VSpace says
     *  where the window is and hands over the root page table it kept
     *  (specs/services.md). Mappings into it may need page tables that do not
     *  exist yet -- unlike the root task's window, nothing promises the region
     *  was ever mapped -- so `tables` is where they come from. */
    bool adopt(seL4_CPtr vspace_root, uintptr_t base, uintptr_t limit,
               Allocator *tables) noexcept;

    /** Map `frame` into the window and return an address we can write, or
     *  nullptr when the window is exhausted or the mapping is refused. */
    void *map(seL4_CPtr frame) noexcept;

    /** The mega-page form: `frame` is a 2 MiB frame (seL4_RISCV_Mega_Page),
     *  mapped at the next 2 MiB-aligned point of the window -- a mega page's
     *  mapping needs a fresh 2 MiB slot, not a page table already carrying
     *  4 KiB leaves (aegir/spawn's window placement says the same of the
     *  child's side). A scanout window is a handful of these rather than
     *  thousands of 4 KiB maps (specs/services.md). */
    void *map_large(seL4_CPtr frame) noexcept;

    /** The kernel's answer to the last map that failed, because "the mapping
     *  was refused" has several causes and they mean different things. */
    uint64_t last_error() const noexcept { return static_cast<uint64_t>(last_error_); }

    /** Map `frame` at exactly `address` instead of at the window cursor. The
     *  address must be page-aligned and inside the window, and this does not
     *  move the cursor -- aegir-heap's caller, which claims the top of the
     *  window and places each page where the allocator behind musl decides
     *  it goes, which is not the cursor's monotonic path. The missing page
     *  tables are created the same way map() creates them, and charged to
     *  the same throwaway account. */
    bool map_at(uintptr_t address, seL4_CPtr frame) noexcept;

    /** Remove one mapping. The frame cap stays ours. Unmapping the *most recently*
     *  mapped frame hands its window page back: filling frames is a strict
     *  map-write-unmap rhythm (child_vspace.cc's populate), and a window that
     *  never recycles is a spawn budget nobody declared. */
    void unmap(seL4_CPtr frame) noexcept;

    /** Put the cursor back to a mark taken with next(). The mappings above the
     *  mark must already be gone: a reclaim revokes the session's pool, the
     *  revoke deletes its frame caps, and the kernel unmaps a mapped frame
     *  when the cap goes (finaliseCap, kernel/src/arch/riscv/object/
     *  objecttype.c) -- so a rewind is bookkeeping only, not unmapping. It is
     *  how a login hands the window pages its spawn staged with back to the
     *  next login: without it the cursor climbs with every login until a map
     *  crosses into table-less window and the FailedLookup table allocation
     *  lands in slots the session's allocator already owns. */
    void rewind(uintptr_t mark) noexcept;

    uintptr_t base() const noexcept { return base_; }
    uintptr_t limit() const noexcept { return limit_; }
    uintptr_t next() const noexcept { return next_; }
    uint64_t mapped_bytes() const noexcept { return mapped_bytes_; }

private:
    seL4_BootInfo *bootinfo_;
    /* The VSpace the window lives in: the kernel's name for the root task's, and
     *  a granted capability for a service's (adopt). */
    seL4_CPtr root_ = seL4_CapInitThreadVSpace;
    /* Where missing page tables come from: for the root task, its allocator
     *  when the window may grow past the tables the image already has
     *  (initialise); for a service, its own allocator (adopt). */
    Allocator *tables_ = nullptr;
    /* The root task's window may extend a large page at a time when it runs
     *  out (initialise); a service's adopted window ends where its spawner
     *  said it ends. */
    bool may_grow_ = false;
    uintptr_t base_;
    uintptr_t next_;
    uintptr_t limit_;
    uint64_t mapped_bytes_;
    /* The most recent mapping, so unmap can give its page back (see unmap). */
    seL4_CPtr last_cap_ = 0;
    seL4_Error last_error_ = seL4_NoError;
};

/** The allocator's node pool, wired to the process's own window. A service (or
 *  the root task) hands its allocator and its Scratch here, and the allocator
 *  grows its bookkeeping from its grant -- carving a frame and mapping it --
 *  instead of a fixed table (specs/allocator.md). The allocator cannot do this
 *  itself: the Scratch depends on the allocator, so the pair is wired here. */
struct NodeWindow {
    Allocator *allocator;
    Scratch *scratch;
};

void *grow_nodes_from_window(void *context, unsigned *bytes) noexcept;

}  // namespace aegir::mem

#endif  // AEGIR_MEM_VSPACE_H