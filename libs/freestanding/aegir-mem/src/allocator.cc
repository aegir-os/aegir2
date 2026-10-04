/*
 * Aegir's own allocator -- implementation. See include/aegir/mem/allocator.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The CSpace addressing used here is the recipe the pinned seL4 tree uses for a
 * single-level root CSpace: the destination is named by the root CNode's own cap
 * at full address depth, because the kernel gives that CNode a guard sized to
 * the rest of the address word
 * (projects/seL4_libs/libsel4allocman/src/bootstrap.c:434-440 sets
 * cnode_guard_bits = seL4_WordBits - cnode_size_bits).
 */

#include <aegir/mem/allocator.h>

namespace aegir::mem {

namespace {
/* Depth at which the root CNode cap itself is addressed. */
constexpr seL4_Word kRootCNodeDepth = seL4_WordBits;
}  // namespace

Allocator::Allocator(seL4_BootInfo *bootinfo) noexcept
    : bootinfo_(bootinfo), node_region_(nullptr), node_capacity_(0), free_nodes_(nullptr),
      node_used_(0), node_free_(0), growing_(false), node_source_(nullptr),
      node_context_(nullptr), cnode_size_bits_(0),
      slots_first_(0), slots_next_(0), slots_end_(0), slots_used_(0), normal_bytes_(0),
      device_bytes_(0), allocated_bytes_(0), last_request_bits_(0), last_candidate_bits_(0)
{
    for (auto &list : heads_) {
        list = nullptr;
    }
    for (auto &list : dev_heads_) {
        list = nullptr;
    }
    adopt_nodes(default_nodes_, sizeof(default_nodes_));
}

void Allocator::adopt_nodes(void *region, unsigned bytes) noexcept
{
    auto *const nodes = static_cast<Node *>(region);
    unsigned const count = bytes / static_cast<unsigned>(sizeof(Node));
    if (nodes == nullptr || count == 0) {
        return;
    }
    node_region_ = nodes;
    node_capacity_ = count;
    free_nodes_ = nullptr;
    node_free_ = count;
    for (unsigned i = 0; i < count; ++i) {
        nodes[i].next = free_nodes_;
        free_nodes_ = &nodes[i];
    }
}

void Allocator::set_node_source(NodeSource source, void *context) noexcept
{
    node_source_ = source;
    node_context_ = context;
}

bool Allocator::initialise() noexcept
{
    if (bootinfo_ == nullptr) {
        return false;
    }

    /* The untyped caps sit in one contiguous run of slots -- a slot region
     * has a start and an end, not a count -- and the kernel describes each of
     * them at the same index (kernel/libsel4/include/sel4/bootinfo_types.h:41-53,
     * :74-75). */
    seL4_Word const untyped_caps = bootinfo_->untyped.end - bootinfo_->untyped.start;
    if (untyped_caps > static_cast<seL4_Word>(CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS)) {
        return false;
    }
    for (seL4_Word i = 0; i < untyped_caps; ++i) {
        seL4_UntypedDesc const &desc = bootinfo_->untypedList[i];
        if (!add_untyped(bootinfo_->untyped.start + i, desc.sizeBits, desc.isDevice != 0,
                         desc.paddr)) {
            return false;
        }
        if (desc.isDevice != 0) {
            device_bytes_ += 1ull << desc.sizeBits;
        } else {
            normal_bytes_ += 1ull << desc.sizeBits;
        }
    }

    /* Slots the kernel left null for us to use (bootinfo_types.h:63). */
    cnode_size_bits_ = static_cast<unsigned>(bootinfo_->initThreadCNodeSizeBits);
    slots_first_ = bootinfo_->empty.start;
    slots_next_ = bootinfo_->empty.start;
    slots_end_ = bootinfo_->empty.end;
    return true;
}

Allocator::Node *Allocator::alloc_node() noexcept
{
    /* Grow with a reserve still in hand: the source carves a frame to map, and
     * carving needs nodes. The flag stops the source's own allocation from
     * asking for another region. */
    constexpr unsigned kReserve = 32;
    if (node_free_ <= kReserve && !growing_ && node_source_ != nullptr) {
        growing_ = true;
        unsigned bytes = 0;
        void *const region = node_source_(node_context_, &bytes);
        growing_ = false;
        auto *const nodes = static_cast<Node *>(region);
        unsigned const count = bytes / static_cast<unsigned>(sizeof(Node));
        for (unsigned i = 0; i < count; ++i) {
            nodes[i].next = free_nodes_;
            free_nodes_ = &nodes[i];
        }
        node_free_ += count;
    }
    Node *node = free_nodes_;
    if (node == nullptr) {
        return nullptr;
    }
    free_nodes_ = node->next;
    --node_free_;
    ++node_used_;
    return node;
}

void Allocator::free_node(Node *node) noexcept
{
    node->next = free_nodes_;
    free_nodes_ = node;
    ++node_free_;
    --node_used_;
}

bool Allocator::add_untyped(seL4_CPtr cap, seL4_Word size_bits, bool device,
                            uint64_t paddr) noexcept
{
    Node *node = alloc_node();
    if (node == nullptr) {
        return false;
    }
    node->cap = cap;
    node->physical = paddr;
    node->size_bits = static_cast<uint8_t>(size_bits);
    node->device = device ? 1 : 0;
    node->parent = nullptr;
    node->sibling = nullptr;
    node->next = nullptr;
    node->prev = nullptr;
    insert(device, node);
    return true;
}

void Allocator::insert(bool device, Node *node) noexcept
{
    Node **const lists = this->lists(device);
    Node **link = &lists[node->size_bits];
    /* Physical order: a piece with a known address goes before the first known
     * address above it, so allocations come off in address order (allocman's
     * reason: contiguous physical memory is friendlier to devices). Unknown
     * addresses -- a delegated untyped whose giver did not say -- go to the
     * head, where they do not pretend to an order they do not have. */
    if (node->physical != 0) {
        while (*link != nullptr && (*link)->physical != 0 &&
               (*link)->physical < node->physical) {
            link = &(*link)->next;
        }
    }
    node->next = *link;
    node->prev = nullptr;
    if (*link != nullptr) {
        (*link)->prev = node;
    }
    *link = node;
    node->free = 1;
    if (trace_ != nullptr) {
        trace_(trace_context_, "insert", node->size_bits, node->physical,
               node->parent != nullptr, node->cap);
    }
}

void Allocator::unlink(Node *node) noexcept
{
    Node **const lists = this->lists(node->device != 0);
    if (node->prev != nullptr) {
        node->prev->next = node->next;
    } else {
        lists[node->size_bits] = node->next;
    }
    if (node->next != nullptr) {
        node->next->prev = node->prev;
    }
    node->next = nullptr;
    node->prev = nullptr;
    node->free = 0;
}

seL4_CPtr Allocator::alloc_slot() noexcept
{
    if (slot_pool_ != nullptr) {
        return slot_pool_->alloc(slot_owner_);
    }
    /* Descending: the cursor is the lowest slot still free, and the free
     * window is [slots_first_, slots_next_). */
    if (slots_descend_) {
        if (slots_next_ <= slots_first_) {
            return 0;
        }
        ++slots_used_;
        return --slots_next_;
    }
    if (slots_next_ >= slots_end_) {
        return 0;
    }
    ++slots_used_;
    return slots_next_++;
}

void Allocator::slot_failed(seL4_CPtr slot) noexcept
{
    if (slot_pool_ != nullptr) {
        slot_pool_->free(slot, slot_owner_);
        return;
    }
    if (slots_descend_) {
        if (slot == slots_next_) {
            ++slots_next_;
            --slots_used_;
        }
        return;
    }
    if (slots_next_ != 0 && slot + 1 == slots_next_) {
        --slots_next_;
        --slots_used_;
    }
}

void Allocator::slot_release(seL4_CPtr mark) noexcept
{
    if (slot_pool_ != nullptr) {
        return;
    }
    /* A descending run is not rewound by a mark: its slots were handed out from
     * the top, and the owner that put them there takes them all back at once
     * (the session pool resets and re-adopts per login, specs/auth.md). */
    if (slots_descend_) {
        return;
    }
    if (mark >= slots_first_ && mark <= slots_next_) {
        slots_used_ -= static_cast<unsigned>(slots_next_ - mark);
        slots_next_ = mark;
    }
}

void Allocator::free_slot(seL4_CPtr slot) noexcept
{
    if (slot_pool_ != nullptr) {
        slot_pool_->free(slot, slot_owner_);
    }
}

void Allocator::reset() noexcept
{
    for (auto &list : heads_) {
        list = nullptr;
    }
    for (auto &list : dev_heads_) {
        list = nullptr;
    }
    /* The provided regions stay ours; only the pieces in them are forgotten. */
    free_nodes_ = nullptr;
    node_used_ = 0;
    node_free_ = node_capacity_;
    growing_ = false;
    for (unsigned i = 0; i < node_capacity_; ++i) {
        node_region_[i].next = free_nodes_;
        free_nodes_ = &node_region_[i];
    }
    slots_first_ = 0;
    slots_next_ = 0;
    slots_end_ = 0;
    slots_used_ = 0;
    allocated_bytes_ = 0;
    last_request_bits_ = 0;
    last_candidate_bits_ = 0;
}

unsigned Allocator::untyped_free() const noexcept
{
    unsigned free = 0;
    for (Node const *list : heads_) {
        for (Node const *node = list; node != nullptr; node = node->next) {
            ++free;
        }
    }
    return free;
}

unsigned Allocator::object_bits(seL4_Word type, seL4_Word size_bits) noexcept
{
    if (type == seL4_CapTableObject) {
        return static_cast<unsigned>(size_bits + seL4_SlotBits);
    }
    return static_cast<unsigned>(size_bits);
}

unsigned Allocator::largest_free_bits() const noexcept
{
    for (unsigned bits = seL4_WordBits; bits > 0; --bits) {
        if (heads_[bits - 1] != nullptr) {
            return bits - 1;
        }
    }
    return 0;
}

unsigned Allocator::check_free_lists() const noexcept
{
    unsigned problems = 0;
    Node *const *const arrays[2] = {heads_, dev_heads_};
    for (Node *const *list : arrays) {
        for (unsigned bits = 0; bits < seL4_WordBits; ++bits) {
            unsigned steps = 0;
            for (Node const *node = list[bits]; node != nullptr; node = node->next) {
                /* A piece belongs in the list of its own size and is marked
                 * free; a walk longer than the pool has a cycle in it. */
                if (node->size_bits != bits || node->free == 0 ||
                    ++steps > node_capacity_) {
                    ++problems;
                    break;
                }
            }
        }
    }
    return problems;
}

Allocator::Piece Allocator::piece_state(void *cookie) const noexcept
{
    Piece out;
    auto const *const node = static_cast<Node *>(cookie);
    if (node == nullptr) {
        return out;
    }
    out.size_bits = node->size_bits;
    out.physical = node->physical;
    out.split_child = node->parent != nullptr;
    out.free = node->free != 0;
    return out;
}

bool Allocator::refill(bool device, seL4_Word size_bits) noexcept
{
    if (refill_inner(device, size_bits)) {
        return true;
    }
    /* The free lists hold nothing at any size. Ask the source for another
     * untyped, adopt it, and search again (specs/memory.md). Device memory has
     * no source: it comes from the bootinfo or nowhere. */
    if (device || !grow_from_source()) {
        return false;
    }
    return refill_inner(device, size_bits);
}

bool Allocator::grow_from_source() noexcept
{
    if (untyped_source_ == nullptr) {
        return false;
    }
    seL4_Word size_bits = 0;
    uint64_t physical = 0;
    seL4_CPtr const cap = untyped_source_(untyped_context_, &size_bits, &physical);
    if (cap == 0 || size_bits == 0) {
        return false;
    }
    return adopt_untyped(cap, size_bits, physical);
}

bool Allocator::refill_inner(bool device, seL4_Word size_bits) noexcept
{
    Node **const lists = this->lists(device);
    /* A listed piece is supposed to be whole. If one is not -- it cannot yield
     * a child -- it leaves the list and the loop looks for the next piece,
     * rather than failing on a node no refill could ever use. */
    for (;;) {
        if (lists[size_bits] != nullptr) {
            return true;
        }
        /* Nothing bigger exists to split: the largest untyped a word can name
         * is one bit below the word. */
        if (size_bits + 1 >= seL4_WordBits) {
            return false;
        }
        if (!refill_inner(device, size_bits + 1)) {
            return false;
        }
        Node *const parent = lists[size_bits + 1];
        if (parent == nullptr) {
            continue;
        }
        /* Take the piece off the list *now*. The node source grows this
         * allocator's pool by allocating from the allocator itself
         * (vspace.cc's `grow_nodes_from_window` calls `alloc_object`), so the
         * `alloc_node` below can re-enter `refill_inner`. Left linked, the
         * re-entrant refill finds this same parent -- still whole -- splits it
         * and spends it, and the retype here is then refused with the kernel's
         * "0 bytes available". Off the list first, the re-entrant call takes a
         * different piece. */
        unlink(parent);
        if (trace_ != nullptr) {
            trace_(trace_context_, "split", parent->size_bits, parent->physical,
                   parent->parent != nullptr, parent->cap);
        }
        Node *const left = alloc_node();
        Node *const right = alloc_node();
        if (left == nullptr || right == nullptr) {
            if (left != nullptr) {
                free_node(left);
            }
            if (right != nullptr) {
                free_node(right);
            }
            /* Nothing was spent: the piece is still whole, so it goes back. */
            insert(device, parent);
            return false;
        }
        seL4_CPtr const left_slot = alloc_slot();
        seL4_CPtr const right_slot = left_slot != 0 ? alloc_slot() : 0;
        if (left_slot == 0 || right_slot == 0) {
            if (left_slot != 0) {
                slot_failed(left_slot);
            }
            if (right_slot != 0) {
                slot_failed(right_slot);
            }
            free_node(left);
            free_node(right);
            insert(device, parent);
            return false;
        }
        /* The parent's two halves: the kernel carves each from the parent's
         * free index, low end first, so the first is the low buddy and the
         * second the high one (kernel/src/object/untyped.c:225-232 aligns the
         * free pointer, :294-302 retypes and moves it). */
        seL4_Error const first = seL4_Untyped_Retype(
            parent->cap, seL4_UntypedObject, size_bits, seL4_CapInitThreadCNode,
            retype_node_index(), cnode_depth_, slot_offset(left_slot), 1);
        if (first != seL4_NoError) {
            /* The piece cannot even yield one child, so it is not whole: it is
             * already off the list, and it stays off. */
            slot_failed(left_slot);
            slot_failed(right_slot);
            free_node(left);
            free_node(right);
            continue;
        }
        seL4_Error const second = seL4_Untyped_Retype(
            parent->cap, seL4_UntypedObject, size_bits, seL4_CapInitThreadCNode,
            retype_node_index(), cnode_depth_, slot_offset(right_slot), 1);
        if (second != seL4_NoError) {
            /* The parent held one child, not two: a retype already spent its
             * low half, so the parent is partial. It is off the list already,
             * and it stays off (a partial parent left listed was the bug,
             * retried forever); the child that did succeed is a whole piece, so
             * it is kept. The rest of the parent is lost; recovering it needs
             * the kernel to say how much is left, which it does not. */
            left->cap = left_slot;
            left->physical = parent->physical;
            left->size_bits = static_cast<uint8_t>(size_bits);
            left->device = parent->device;
            left->parent = nullptr;
            left->sibling = nullptr;
            left->next = nullptr;
            left->prev = nullptr;
            slot_failed(right_slot);
            free_node(right);
            insert(device, left);
            return true;
        }
        /* Both halves exist. The parent is the merge anchor: it is off the
         * free list already, and its memory is reclaimable once both children
         * are deleted (the kernel resets a childless untyped's free index on
         * the next retype, kernel/src/object/untyped.c:184-189). */
        left->cap = left_slot;
        left->physical = parent->physical;
        left->size_bits = static_cast<uint8_t>(size_bits);
        left->device = parent->device;
        left->parent = parent;
        left->sibling = right;
        left->next = nullptr;
        left->prev = nullptr;
        right->cap = right_slot;
        right->physical =
            parent->physical != 0 ? parent->physical + (1ull << size_bits) : 0;
        right->size_bits = static_cast<uint8_t>(size_bits);
        right->device = parent->device;
        right->parent = parent;
        right->sibling = left;
        right->next = nullptr;
        right->prev = nullptr;
        insert(device, right);
        insert(device, left);
        return true;
    }
}

Allocator::Node *Allocator::take(bool device, seL4_Word size_bits) noexcept
{
    Node **const lists = this->lists(device);
    Node *const node = lists[size_bits];
    if (node == nullptr) {
        return nullptr;
    }
    unlink(node);
    if (trace_ != nullptr) {
        trace_(trace_context_, "take", node->size_bits, node->physical,
               node->parent != nullptr, node->cap);
    }
    return node;
}

void Allocator::free_piece(Node *node) noexcept
{
    /* The piece goes back whole whatever the caller did. A caller hands a piece
     * back with `Revoke`, never `Delete` -- a delete leaves its derived caps
     * alive -- and a caller that did not would otherwise put a spent piece on
     * the free list. Revoking here makes the invariant the allocator's, not the
     * caller's (specs/memory.md). */
    ensure_piece_whole(node);
    /* A piece whose buddy is free merges with it: delete both children (so the
     * parent has none and its memory resets), return their node slots, and give
     * the parent back in turn (allocman's `_utspace_split_free`). */
    if (node->parent != nullptr && node->sibling != nullptr && node->sibling->free != 0) {
        Node *const sibling = node->sibling;
        Node *const parent = node->parent;
        unlink(sibling);
        /* Deleting a *slot* addresses it at the CNode's radix, not the
         * retype's node depth: a service retypes at depth zero but deletes at
         * its CNode's size (specs/memory.md). */
        seL4_Word const del_depth =
            cnode_size_bits_ != 0 ? cnode_size_bits_ : cnode_depth_;
        /* Revoke before delete: a delete leaves the piece's derived caps alive,
         * so a caller that handed the piece back without revoking the objects it
         * retyped from it leaves the memory in use while the merge re-lists the
         * parent -- a free piece whose memory is spent, which the next retype
         * from it refuses ("0 bytes available"). The revoke makes the piece's
         * memory free whatever the caller did (specs/memory.md). */
        seL4_CNode_Revoke(seL4_CapInitThreadCNode, node->cap, del_depth);
        seL4_CNode_Revoke(seL4_CapInitThreadCNode, sibling->cap, del_depth);
        seL4_CNode_Delete(seL4_CapInitThreadCNode, node->cap, del_depth);
        seL4_CNode_Delete(seL4_CapInitThreadCNode, sibling->cap, del_depth);
        /* The caps are gone, so their slots are free again -- and a pool that
         * remembers owners must be told (specs/memory.md). */
        free_slot(node->cap);
        free_slot(sibling->cap);
        free_node(sibling);
        free_node(node);
        free_piece(parent);
    } else {
        insert(node->device != 0, node);
    }
}

void Allocator::ensure_piece_whole(Node *node) noexcept
{
    if (node->device != 0) {
        /* A device untyped's frames are carved by address, not a free index,
         * and the frames a caller keeps must not be revoked out from under it. */
        return;
    }
    /* Revoke deletes every capability derived from the piece's cap, so the
     * piece is childless and the kernel will reset its free index on the next
     * retype (kernel/src/object/untyped.c:182-189). It is a no-op when the
     * piece is already whole, and it costs no slot. */
    seL4_Word const del_depth =
        cnode_size_bits_ != 0 ? cnode_size_bits_ : cnode_depth_;
    seL4_CNode_Revoke(seL4_CapInitThreadCNode, node->cap, del_depth);
}

bool Allocator::free_object(void *cookie, seL4_Word size_bits) noexcept
{
    if (cookie == nullptr) {
        return false;
    }
    auto *const node = static_cast<Node *>(cookie);
    if (node->size_bits != size_bits || node->free != 0) {
        return false;
    }
    free_piece(node);
    return true;
}

seL4_CPtr Allocator::alloc_object(seL4_Word type, seL4_Word size_bits, Account &account,
                                  seL4_Error *error, void **cookie) noexcept
{
    *error = seL4_NoError;
    unsigned const wanted = object_bits(type, size_bits);
    last_request_bits_ = static_cast<unsigned>(size_bits);
    last_candidate_bits_ = 0;
    if (!refill(false, wanted)) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    Node *const node = take(false, wanted);
    if (node == nullptr) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    last_candidate_bits_ = node->size_bits;
    seL4_CPtr const slot = alloc_slot();
    if (slot == 0) {
        insert(false, node);
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    *error = seL4_Untyped_Retype(node->cap, type, size_bits, seL4_CapInitThreadCNode,
                                 retype_node_index(), cnode_depth_, slot_offset(slot), 1);
    if (*error != seL4_NoError) {
        slot_failed(slot);
        /* The piece cannot yield the object, so it is not a usable piece: it is
         * dropped, not put back. Putting a piece back whose retype was refused
         * -- a spent piece -- leaves it on the free list for the next caller,
         * and that is how a spent piece reaches a carve (specs/memory.md). The
         * piece is already off the list (take), so this only gives its node
         * back. */
        free_node(node);
        return 0;
    }
    /* The piece is the object now. The node stays as the object's identity: a
     * caller that will free it takes the cookie, and a caller that will not
     * leaves it, which keeps the merge tree whole for the pieces around it (a
     * node whose storage went back could be reused under its buddy's feet). */
    if (cookie != nullptr) {
        *cookie = node;
    }
    account.bytes += 1ull << wanted;
    account.objects += 1;
    allocated_bytes_ += 1ull << wanted;
    return slot;
}

seL4_CPtr Allocator::alloc_slot_run(seL4_Word count) noexcept
{
    if (slot_pool_ != nullptr) {
        return slot_pool_->alloc_run(count, slot_owner_);
    }
    if (slots_descend_) {
        if (slots_next_ < slots_first_ + count) {
            return 0;
        }
        slots_next_ -= count;
        slots_used_ += count;
        return slots_next_;
    }
    if (slots_next_ + count > slots_end_) {
        return 0;
    }
    seL4_CPtr const first = slots_next_;
    slots_next_ += count;
    slots_used_ += count;
    return first;
}

void Allocator::slot_failed_run(seL4_CPtr first, seL4_Word count) noexcept
{
    if (slot_pool_ != nullptr) {
        for (seL4_Word i = 0; i < count; ++i) {
            slot_pool_->free(first + i, slot_owner_);
        }
        return;
    }
    if (slots_descend_) {
        if (first == slots_next_) {
            slots_next_ += count;
            slots_used_ -= count;
        }
        return;
    }
    if (first + count == slots_next_) {
        slots_next_ -= count;
        slots_used_ -= count;
    }
}

seL4_CPtr Allocator::alloc_pages_run(seL4_Word count, Account &account, seL4_Error *error,
                                     void **cookie) noexcept
{
    *error = seL4_NoError;
    /* A run is a whole buddy piece, so it is a power of two frames: a retype
     * fills a consecutive destination run, and a piece is a power of two. */
    if (count == 0 || (count & (count - 1)) != 0) {
        *error = seL4_InvalidArgument;
        return 0;
    }
    unsigned const page_bits = object_bits(seL4_RISCV_4K_Page, seL4_PageBits);
    unsigned log = 0;
    for (seL4_Word n = count; n > 1; n >>= 1) {
        ++log;
    }
    unsigned const wanted = page_bits + log;
    last_request_bits_ = page_bits;
    last_candidate_bits_ = 0;
    if (!refill(false, wanted)) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    Node *const node = take(false, wanted);
    if (node == nullptr) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    last_candidate_bits_ = node->size_bits;
    seL4_CPtr const first = alloc_slot_run(count);
    if (first == 0) {
        insert(false, node);
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    *error = seL4_Untyped_Retype(node->cap, seL4_RISCV_4K_Page, seL4_PageBits,
                                 seL4_CapInitThreadCNode, retype_node_index(), cnode_depth_,
                                 slot_offset(first), count);
    if (*error != seL4_NoError) {
        slot_failed_run(first, count);
        free_node(node);
        return 0;
    }
    if (cookie != nullptr) {
        *cookie = node;
    }
    account.bytes += static_cast<uint64_t>(count) << page_bits;
    account.objects += count;
    allocated_bytes_ += static_cast<uint64_t>(count) << page_bits;
    return first;
}

bool Allocator::device_window(uint64_t base_paddr, unsigned pages, seL4_CPtr *first_out,
                              seL4_Error *error) noexcept
{
    *first_out = 0;
    *error = seL4_NoError;
    if (bootinfo_ == nullptr || pages == 0) {
        *error = seL4_InvalidArgument;
        return false;
    }
    /* The untyped the kernel described as device memory *and* that covers the address
     * asked for: device memory arrives as untypeds like any other, marked as device
     * (seL4_UntypedDesc::isDevice), and a device frame can come from nowhere else. */
    seL4_Word const count = bootinfo_->untyped.end - bootinfo_->untyped.start;
    for (seL4_Word i = 0; i < count; ++i) {
        seL4_UntypedDesc const &desc = bootinfo_->untypedList[i];
        if (desc.isDevice == 0) {
            continue;
        }
        uint64_t const base = desc.paddr;
        uint64_t const need = static_cast<uint64_t>(pages) << seL4_PageBits;
        if (base_paddr < base || base_paddr - base + need > (1ull << desc.sizeBits)) {
            continue;
        }
        for (unsigned page = 0; page < pages; ++page) {
            seL4_CPtr const slot = alloc_slot();
            if (slot == 0) {
                *error = seL4_NotEnoughMemory;
                return false;
            }
            seL4_Error const retyped =
                seL4_Untyped_Retype(bootinfo_->untyped.start + i, seL4_RISCV_4K_Page,
                                    seL4_PageBits, seL4_CapInitThreadCNode,
                                    retype_node_index(), cnode_depth_, slot_offset(slot), 1);
            if (retyped != seL4_NoError) {
                slot_failed(slot);
                *error = retyped;
                return false;
            }
            if (page == 0) {
                *first_out = slot;
            }
        }
        return true;
    }
    *error = seL4_InvalidArgument;
    return false;
}

bool Allocator::adopt_untyped(seL4_CPtr cap, seL4_Word size_bits, uint64_t paddr) noexcept
{
    return add_untyped(cap, size_bits, false, paddr);
}

void Allocator::adopt_slots(seL4_CPtr first, seL4_Word count, seL4_Word depth,
                            seL4_Word radix) noexcept
{
    slots_first_ = first;
    slots_next_ = first;
    slots_end_ = first + count;
    slots_descend_ = false;
    slots_used_ = 0;
    cnode_depth_ = depth;
    cnode_size_bits_ = static_cast<unsigned>(radix);
    level_two_bits_ = 0;
    cnode_index_ = 0;
}

void Allocator::adopt_slots_down(seL4_CPtr first, seL4_Word count, seL4_Word depth,
                                 seL4_Word radix) noexcept
{
    slots_first_ = first;
    slots_next_ = first + count;
    slots_end_ = first + count;
    slots_descend_ = true;
    slots_used_ = 0;
    cnode_depth_ = depth;
    cnode_size_bits_ = static_cast<unsigned>(radix);
    level_two_bits_ = 0;
    cnode_index_ = 0;
}

void Allocator::adopt_slots_level_two(seL4_CPtr first, seL4_Word count, seL4_Word l1,
                                      seL4_Word l2, seL4_Word cnode_index) noexcept
{
    level_two_bits_ = l2;
    cnode_index_ = cnode_index;
    /* Both CNodes have guard zero, so a cap op addresses an L2 slot at
     * l1 + l2, while a retype names the L2 CNode cap at depth l1 against the
     * guard-zero own-CNode cap (specs/memory.md). */
    cnode_depth_ = l1;
    cnode_size_bits_ = static_cast<unsigned>(l1 + l2);
    /* The cursor lives in encoded space; because first + count stays inside one
     * L2 CNode, encoding is additive there (adopt_slots_level_two explains). */
    seL4_CPtr const base = static_cast<seL4_CPtr>((cnode_index << l2) | first);
    slots_first_ = base;
    slots_next_ = base;
    slots_end_ = base + count;
    slots_descend_ = false;
    slots_used_ = 0;
}

seL4_CPtr Allocator::carve_untyped(seL4_Word size_bits, Account &account, seL4_Error *error,
                                   uint64_t *physical_out, void **cookie) noexcept
{
    *error = seL4_NoError;
    if (!refill(false, size_bits)) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    Node *const node = take(false, size_bits);
    if (node == nullptr) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    /* What is handed out must have nothing derived from it -- the kernel will
     * not let the caller copy a capability with derived objects ("RevokeFirst:
     * The untyped has been used to retype an object",
     * out/aegir/libsel4/include/interfaces/sel4_client.h:419), and a piece that
     * is not childless is spent, so a retype from it would be refused with the
     * kernel's "0 bytes available". A listed piece is meant to be childless
     * already; revoking makes it so whatever a caller did, so the handout is
     * whole (specs/memory.md). */
    ensure_piece_whole(node);
    if (physical_out != nullptr) {
        *physical_out = node->physical;
    }
    seL4_CPtr const cap = node->cap;
    if (cookie != nullptr) {
        *cookie = node;
    }
    account.bytes += 1ull << size_bits;
    account.objects += 1;
    allocated_bytes_ += 1ull << size_bits;
    return cap;
}

seL4_CPtr Allocator::carve_page(seL4_CPtr untyped_cap, Account &account,
                                seL4_Error *error, seL4_Word size_bits) noexcept
{
    *error = seL4_NoError;
    seL4_CPtr const slot = alloc_slot();
    if (slot == 0) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    /* The object type names the frame's size (kernel/src/arch/riscv/object/
     * objecttype.c: seL4_RISCV_Mega_Page is a 2 MiB frame); the size_bits the
     * retype also carries are ignored for a fixed-size type, so they simply
     * agree. */
    seL4_Word const type =
        size_bits == seL4_PageBits ? seL4_RISCV_4K_Page : seL4_RISCV_Mega_Page;
    seL4_Error const retyped =
        seL4_Untyped_Retype(untyped_cap, type, size_bits, seL4_CapInitThreadCNode,
                            retype_node_index(), cnode_depth_, slot_offset(slot), 1);
    if (retyped != seL4_NoError) {
        slot_failed(slot);
        *error = retyped;
        return 0;
    }
    account.objects += 1;
    return slot;
}

seL4_CPtr Allocator::alloc_page(Account &account, seL4_Error *error,
                                seL4_Word size_bits) noexcept
{
    /* The object type names the frame's size, the rule carve_page states
     * above; the size_bits the retype also carries agree. */
    seL4_Word const type =
        size_bits == seL4_PageBits ? seL4_RISCV_4K_Page : seL4_RISCV_Mega_Page;
    return alloc_object(type, size_bits, account, error);
}

seL4_CPtr Allocator::alloc_cnode_at_l1(seL4_Word l2_bits, seL4_Word l1_slot,
                                       Account &account, seL4_Error *error,
                                       void **cookie) noexcept
{
    *error = seL4_NoError;
    unsigned const wanted = object_bits(seL4_CapTableObject, l2_bits);
    last_request_bits_ = static_cast<unsigned>(l2_bits);
    last_candidate_bits_ = 0;
    if (!refill(false, wanted)) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    Node *const node = take(false, wanted);
    if (node == nullptr) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    last_candidate_bits_ = node->size_bits;
    /* `node_depth` zero names the root CNode itself (kernel/src/object/
     * untyped.c:113), so the object lands in root slot `l1_slot`: the L2 CNode
     * cap *is* that slot, which is what a two-level address names
     * (specs/memory.md). */
    *error = seL4_Untyped_Retype(node->cap, seL4_CapTableObject, l2_bits,
                                 seL4_CapInitThreadCNode, seL4_CapInitThreadCNode, 0,
                                 l1_slot, 1);
    if (*error != seL4_NoError) {
        free_node(node);
        return 0;
    }
    if (cookie != nullptr) {
        *cookie = node;
    }
    account.bytes += 1ull << wanted;
    account.objects += 1;
    allocated_bytes_ += 1ull << wanted;
    return static_cast<seL4_CPtr>(l1_slot);
}

seL4_CPtr Allocator::make_asid_pool(Account &account, seL4_Error *error) noexcept
{
    *error = seL4_NoError;
    /* A page is comfortably more than a pool needs, and the kernel refuses anything
     * smaller -- so if this is ever wrong, it is wrong out loud. */
    seL4_CPtr const untyped = carve_untyped(seL4_PageBits, account, error);
    if (untyped == 0) {
        return 0;
    }
    seL4_CPtr const pool = alloc_slot();
    if (pool == 0) {
        *error = seL4_NotEnoughMemory;
        return 0;
    }
    *error = seL4_RISCV_ASIDControl_MakePool(seL4_CapASIDControl, untyped,
                                             seL4_CapInitThreadCNode, pool, kRootCNodeDepth);
    if (*error != seL4_NoError) {
        return 0;
    }
    account.objects += 1;
    return pool;
}

}  // namespace aegir::mem
