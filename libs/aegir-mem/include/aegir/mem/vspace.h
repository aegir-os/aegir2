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

    /** Work out the window from the bootinfo. */
    bool initialise() noexcept;

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

    /** Remove one mapping. The frame cap stays ours. Unmapping the *most recently*
     *  mapped frame hands its window page back: filling frames is a strict
     *  map-write-unmap rhythm (child_vspace.cc's populate), and a window that
     *  never recycles is a spawn budget nobody declared. */
    void unmap(seL4_CPtr frame) noexcept;

    uintptr_t base() const noexcept { return base_; }
    uintptr_t limit() const noexcept { return limit_; }
    uintptr_t next() const noexcept { return next_; }
    uint64_t mapped_bytes() const noexcept { return mapped_bytes_; }

private:
    seL4_BootInfo *bootinfo_;
    /* The VSpace the window lives in: the kernel's name for the root task's, and
     *  a granted capability for a service's (adopt). */
    seL4_CPtr root_ = seL4_CapInitThreadVSpace;
    /* Where missing page tables come from: null for the root task, whose window
     *  sits where the tables already exist, and the service's own allocator
     *  otherwise. */
    Allocator *tables_ = nullptr;
    uintptr_t base_;
    uintptr_t next_;
    uintptr_t limit_;
    uint64_t mapped_bytes_;
    /* The most recent mapping, so unmap can give its page back (see unmap). */
    seL4_CPtr last_cap_ = 0;
};

}  // namespace aegir::mem

#endif  // AEGIR_MEM_VSPACE_H
