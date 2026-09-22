/*
 * Aegir's own allocator: capability slots and untyped memory.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The root task is handed the machine -- every untyped capability the kernel
 * enumerated, plus the empty slots in its own CSpace (specs/director.md). This
 * turns that grant into objects. It is deliberately ours rather than
 * libsel4utils': see specs/userland.md for why, and specs/director.md for the
 * spawn path it feeds.
 *
 * Two properties are enforced here rather than assumed:
 *
 *   - Nothing is invented. The untyped list, the free slot range and the sizes
 *     all come from the bootinfo or from the kernel's own constants; there is no
 *     capacity table of ours to keep in sync (project rule: capacity tables grow
 *     on demand). When the slot range runs out, allocation fails loudly and
 *     growing the CSpace is the next piece of work -- not a number quietly
 *     chosen here.
 *   - Everything is charged. Every object is retyped out of an untyped an
 *     account holds, so accounting is a side effect of allocating rather than a
 *     second bookkeeping pass (specs/authority.md).
 */

#ifndef AEGIR_MEM_ALLOCATOR_H
#define AEGIR_MEM_ALLOCATOR_H

#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::mem {

/** What an allocation is charged to (specs/authority.md). */
struct Account {
    char const *name;
    uint64_t bytes;
    uint64_t objects;
    uint64_t slots;
};

class Allocator {
public:
    explicit Allocator(seL4_BootInfo *bootinfo) noexcept;

    /** Index the untyped memory and the free slots the kernel reported. */
    bool initialise() noexcept;

    /**
     * A free slot in our CSpace root, or 0 when the range is exhausted.
     *
     * This *reserves* it: the cursor moves, and `slot_failed` gives it back if what
     * the slot was reserved for does not happen. Nothing here can free a slot that has
     * been used -- see the deferral in specs/userland.md -- so a slot lost to a failed
     * allocation is lost for good, which is why the failure paths give theirs back.
     */
    seL4_CPtr alloc_slot() noexcept;

    /**
     * The slot cursor as it stands, for handing a whole range back at once.
     *
     * `alloc_slot` above says a used slot is never freed, and that stays true
     * one slot at a time. The exception is a range the *kernel* has emptied:
     * when every capability past a mark was retyped from one untyped, revoking
     * that untyped deletes them all (authority.md's retained-copy path), and
     * the slots are empty again whether this allocator says so or not. The
     * session-reclaim arc is the caller (specs/auth.md): mark at login, spawn,
     * revoke, release.
     */
    seL4_CPtr slot_mark() const noexcept { return slots_next_; }

    /**
     * Move the cursor back to a mark taken with `slot_mark`, returning every
     * slot past it to the free range. Valid only when a revoke has emptied
     * those slots -- releasing a range with live capabilities in it hands the
     * same slot out twice, and the kernel reports the second occupant as
     * `seL4_DeleteFirst`, "the destination slot is occupied". A mark outside
     * the range, or ahead of the cursor, is ignored rather than acted on.
     */
    void slot_release(seL4_CPtr mark) noexcept;

    /**
     * Forget every untyped record and the slot range, so the allocator can be
     * re-adopted over a pool a revoke has made whole again (specs/auth.md's
     * Session reclaim). Deletes nothing: the records this drops belong to
     * capabilities the revoke already took, which is the only time forgetting
     * them is right.
     */
    void reset() noexcept;

    /**
     * Retype one object of `type`/`size_bits` out of untyped memory, splitting a
     * larger untyped when that is what is available, and charge it to `account`.
     * Returns 0 and writes `*error` on failure.
     *
     * `size_bits` is what `seL4_Untyped_Retype` wants for that type, which is not
     * always the memory the object costs: the allocator works the memory out with
     * `object_bits` so that a caller does not have to know the kernel's rule.
     */
    seL4_CPtr alloc_object(seL4_Word type, seL4_Word size_bits, Account &account,
                           seL4_Error *error, void **cookie = nullptr) noexcept;

    /**
     * A window of device memory: `pages` frames, for the device registers at
     * `base_paddr` and the pages after it.
     *
     * A device frame can only be retyped from the device untyped that covers its
     * address, and a retype takes no interior offset -- it carves from the untyped's own
     * cursor, which only moves forwards (kernel/src/object/untyped.c,
     * `decodeUntypedInvocation`). So reaching a device's page means retyping every page
     * before it, and the pages along the way are **kept**: a frame that is dropped goes
     * back to its untyped and the next retype carves it again, which is how asking for a
     * device's page and getting the untyped's first page instead became a bug that
     * looked like "the device answers zeros".
     *
     * `*first_out` is the first of `pages` consecutive capabilities, one per page. This
     * is the *delegator's* call -- it needs the bootinfo's untyped descriptions, which a
     * service does not have -- so a service is given a window rather than asked to find
     * one (specs/services.md).
     */
    bool device_window(uint64_t base_paddr, unsigned pages, seL4_CPtr *first_out,
                       seL4_Error *error) noexcept;

    /**
     * A raw untyped capability of exactly `size_bits`, carved out and given away.
     *
     * Retyping *objects* is what the rest of this class does; this is for the ones
     * the kernel makes from an untyped directly rather than by retyping -- an ASID
     * pool, which `seL4_RISCV_ASIDControl_MakePool` builds from an untyped of its
     * own. The caller owns the capability and may pass it to another process, which
     * is how spawing authority is delegated (specs/authority.md). The record it came
     * from is marked used, so the memory is not handed out twice.
     *
     * What comes back is always a capability the kernel will let the caller *copy*:
     * a capability with derived objects cannot be (`seL4_RevokeFirst`), and a parent
     * that was split has them. So when the region had to be split out of something
     * larger, the handout is the last leaf the splitting made -- which is exactly
     * `size_bits` wide and has nothing derived from it -- and the parent's remainder
     * stays here, free for later allocations. When an exact-size untyped existed it
     * is handed over as it stands.
     *
     * `physical_out`, when given, is set to the region's *physical* base -- the one
     * thing a service cannot find out for itself and a device has to be told, because
     * a virtqueue's descriptor entries are guest-physical addresses
     * (specs/services.md).
     */
    seL4_CPtr carve_untyped(seL4_Word size_bits, Account &account, seL4_Error *error,
                            uint64_t *physical_out = nullptr,
                            void **cookie = nullptr) noexcept;

    /**
     * Adopt an untyped this process was *handed* rather than one it found in its own
     * bootinfo, so a service can retype objects out of delegated memory. False when
     * there is no room left to remember it. (specs/authority.md)
     *
     * `paddr` is where the region is in the machine, when the giver said: there is
     * no invocation that reads an untyped's address, so a region a device will be
     * pointed at has to arrive with its address attached -- zero means unknown, and
     * nothing derived from this region should be named to a device.
     */
    bool adopt_untyped(seL4_CPtr cap, seL4_Word size_bits, uint64_t paddr = 0) noexcept;

    /**
     * A page retyped out of an untyped this allocator handed out, put in a slot of its own.
     * `carve_untyped` gives away the authority over a region; this gives back something a
     * caller can use, which is what a service's memory grant needs to become a frame the
     * spawner can map into it. The region's physical base is the page's, because an untyped
     * of exactly `seL4_PageBits` holds exactly one page.
     *
     * `size_bits` is seL4_PageBits (4 KiB) or seL4_LargePageBits (2 MiB, a
     * seL4_RISCV_Mega_Page): a window big enough to hold a framebuffer is a
     * handful of mega pages, not a thousand small ones -- every cap in a set is
     * a slot somebody's CSpace pays for (specs/services.md). Consecutive carves
     * stay aligned because the untyped's watermark advances by the frame's size.
     */
    seL4_CPtr carve_page(seL4_CPtr untyped_cap, Account &account, seL4_Error *error,
                         seL4_Word size_bits = seL4_PageBits) noexcept;

    /**
     * Adopt a run of slots this process may put capabilities in, and the depth that
     * addresses them. A service's CSpace is its own -- the slots the block did not
     * name are nobody else's -- and its allocator has to be told where they are and
     * how to reach them, the way director's is told by the kernel. `depth` is the
     * whole word for the kernel's root CNode; a *service* addresses its own slots
     * with depth zero, where the destination capability *is* the CNode
     * (kernel/src/object/untyped.c, `decodeUntypedInvocation`).
     */
    void adopt_slots(seL4_CPtr first, seL4_Word count, seL4_Word depth) noexcept;

    /**
     * An ASID pool, for a process that will build address spaces of its own.
     *
     * Not `alloc_object`: the kernel makes a pool from an *untyped* rather than by
     * retyping one (seL4_RISCV_ASIDControl_MakePool), so the memory is consumed and
     * the capability that comes back is the pool. This is where the
     * architecture-specific call lives, so a caller does not have to know which
     * architecture it is on (AGENTS.md, and specs/authority.md for what it is for).
     */
    seL4_CPtr make_asid_pool(Account &account, seL4_Error *error) noexcept;

    /**
     * The memory an object of `type`/`size_bits` needs, in bits -- the kernel's
     * own rule (kernel/src/object/objecttype.c:42-48). A CNode is the case that
     * bites: its retype size is the number of slot bits, but it costs those plus
     * seL4_SlotBits of capability bookkeeping, so an allocator that splits to the
     * number it was handed asks the kernel for five more bits of memory than it
     * reserved and is refused.
     */
    static unsigned object_bits(seL4_Word type, seL4_Word size_bits) noexcept;

    /**
     * Give an object's memory back: the piece it was retyped from goes back to
     * its free list, merging with its buddy when the buddy is free (the buddy
     * tree is what makes that possible). `cookie` is what an allocation
     * returned through its `cookie` out-parameter -- the capability a caller
     * holds is the *object*, not the piece, and only the allocator knows which
     * piece an object came from. False when the cookie is not one of ours.
     */
    bool free_object(void *cookie, seL4_Word size_bits) noexcept;

    /**
     * The node pool: mapped regions the allocator turns into bookkeeping nodes.
     * It cannot map its own -- the `Scratch` does that, and the `Scratch`
     * depends on the allocator -- so the caller provides the first region and a
     * way to get another when it runs low. Growth is bounded by the untyped the
     * process was given, so it is not unbounded. `region` must be a mapped,
     * writable run of at least `count * sizeof(Node)` bytes.
     */
    using NodeSource = void *(*)(void *context, unsigned *bytes);
    void adopt_nodes(void *region, unsigned bytes) noexcept;
    void set_node_source(NodeSource source, void *context) noexcept;

    /* The last failed object allocation, for the boot report: a spawn that fails
     * should say what it wanted and what it found. */
    unsigned last_request_bits() const noexcept { return last_request_bits_; }
    unsigned last_candidate_bits() const noexcept { return last_candidate_bits_; }

    /** The largest piece still free to allocate from. */
    unsigned largest_free_bits() const noexcept;

    /* What the machine gave us, for the boot report. */
    unsigned untyped_count() const noexcept { return node_used_; }
    unsigned untyped_free() const noexcept;
    uint64_t normal_bytes() const noexcept { return normal_bytes_; }
    uint64_t device_bytes() const noexcept { return device_bytes_; }
    uint64_t allocated_bytes() const noexcept { return allocated_bytes_; }
    unsigned cnode_size_bits() const noexcept { return cnode_size_bits_; }
    unsigned slots_total() const noexcept { return slots_end_ - slots_first_; }
    unsigned slots_used() const noexcept { return slots_used_; }

private:
    /* One piece of an untyped: the capability, its size, where it is, and the
     * buddy tree that lets a free merge it back with the piece it was split
     * from. The shape is libsel4allocman's `utspace_split_node`
     * (projects/seL4_libs/libsel4allocman/src/utspace/split.c, BSD-2-Clause);
     * what is Aegir's is the storage (a static pool, not an mspace) and the
     * capabilities (slots in one CSpace, not `cspacepath`s). */
    struct Node {
        seL4_CPtr cap;
        /* Where the piece's memory is in the machine, or 0 when the giver did
         * not say. Splitting moves it: the kernel carves a child from the low
         * end, so the buddy above it begins one size higher. */
        uint64_t physical;
        uint8_t size_bits;
        uint8_t device;
        uint8_t free;
        /* The piece this was split from (null for a root), and the other half
         * of that split. A free merges the two and frees the parent in turn. */
        Node *parent;
        Node *sibling;
        /* The free-list links; `next` doubles as the node-pool link when the
         * node is not a piece. */
        Node *next;
        Node *prev;
    };

    /** A free node slot, or nullptr when the pool is exhausted. */
    Node *alloc_node() noexcept;
    void free_node(Node *node) noexcept;

    /** Put a root untyped into its class's size list, in physical order. False
     *  when the node pool is full. */
    bool add_untyped(seL4_CPtr cap, seL4_Word size_bits, bool device, uint64_t paddr) noexcept;

    /** The free lists of a class. */
    Node **lists(bool device) noexcept { return device ? dev_heads_ : heads_; }

    /** Make sure `size_bits` has a free piece, splitting a larger one into two
     *  equal buddies until it does (allocman's `_refill_pool`). */
    bool refill(bool device, seL4_Word size_bits) noexcept;

    /** Unlink a free piece and mark it allocated, or nullptr when none. */
    Node *take(bool device, seL4_Word size_bits) noexcept;

    /** Insert a free piece into its size list, in physical order. */
    void insert(bool device, Node *node) noexcept;

    /** Unlink a free piece from its list. */
    void unlink(Node *node) noexcept;

    /** Give a piece back, merging with its buddy when the buddy is free
     *  (allocman's `_utspace_split_free`). */
    void free_piece(Node *node) noexcept;

    /** Give back the most recent reservation, if `slot` is it. A cursor that keeps
     *  walking past a failed retype leaves it behind the slots actually in use, and a
     *  later allocation lands on top of one (seL4_DeleteFirst, "the destination slot is
     *  occupied"). Only the last reservation can be returned, which is all these paths
     *  need: one slot is outstanding at a time. */
    void slot_failed(seL4_CPtr slot) noexcept;

    seL4_BootInfo *bootinfo_;
    /* The node pool: regions the caller provided, linked as one free list. A
     * full pool asks `node_source_` for another region rather than failing --
     * the caller carves and maps it, the allocator only retypes. */
    Node *node_region_;
    unsigned node_capacity_;
    Node *free_nodes_;
    unsigned node_used_;
    /* How many nodes are free, and whether a grow is in progress. The source
     * itself allocates (a frame to map), so `alloc_node` must not ask for
     * another region while it is being grown -- hence the flag -- and it grows
     * with a reserve still in hand, because the frame the source carves needs
     * nodes of its own. */
    unsigned node_free_;
    bool growing_;
    NodeSource node_source_;
    void *node_context_;
    /* A small built-in region, so an allocator whose caller does not adopt one
     * still works; a caller that knows its grant adopts a bigger one and/or a
     * source, and this is not touched. It is a floor, not the size. */
    Node default_nodes_[CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS * 8];
    Node *heads_[seL4_WordBits];
    Node *dev_heads_[seL4_WordBits];
    unsigned cnode_size_bits_;
    /* The depth that addresses our slots: the whole word for the kernel's root CNode,
     * and zero for a service that addresses its own CSpace (adopt_slots explains). */
    seL4_Word cnode_depth_ = seL4_WordBits;
    seL4_CPtr slots_first_;
    seL4_CPtr slots_next_;
    seL4_CPtr slots_end_;
    unsigned slots_used_;
    uint64_t normal_bytes_;
    uint64_t device_bytes_;
    uint64_t allocated_bytes_;
    unsigned last_request_bits_;
    unsigned last_candidate_bits_;
};

}  // namespace aegir::mem

#endif  // AEGIR_MEM_ALLOCATOR_H
