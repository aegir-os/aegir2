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
    if (slots_next_ >= slots_end_) {
        return 0;
    }
    ++slots_used_;
    return slots_next_++;
}

void Allocator::slot_failed(seL4_CPtr slot) noexcept
{
    if (slots_next_ != 0 && slot + 1 == slots_next_) {
        --slots_next_;
        --slots_used_;
    }
}

void Allocator::slot_release(seL4_CPtr mark) noexcept
{
    if (mark >= slots_first_ && mark <= slots_next_) {
        slots_used_ -= static_cast<unsigned>(slots_next_ - mark);
        slots_next_ = mark;
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

bool Allocator::refill(bool device, seL4_Word size_bits) noexcept
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
        if (!refill(device, size_bits + 1)) {
            return false;
        }
        Node *const parent = lists[size_bits + 1];
        if (parent == nullptr) {
            continue;
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
            return false;
        }
        /* The parent's two halves: the kernel carves each from the parent's
         * free index, low end first, so the first is the low buddy and the
         * second the high one (kernel/src/object/untyped.c:225-232 aligns the
         * free pointer, :294-302 retypes and moves it). */
        seL4_Error const first = seL4_Untyped_Retype(
            parent->cap, seL4_UntypedObject, size_bits, seL4_CapInitThreadCNode,
            seL4_CapInitThreadCNode, cnode_depth_, left_slot, 1);
        if (first != seL4_NoError) {
            /* The piece cannot even yield one child, so it is not whole: it
             * leaves the list, or every refill would retry it and fail. */
            unlink(parent);
            slot_failed(left_slot);
            slot_failed(right_slot);
            free_node(left);
            free_node(right);
            continue;
        }
        seL4_Error const second = seL4_Untyped_Retype(
            parent->cap, seL4_UntypedObject, size_bits, seL4_CapInitThreadCNode,
            seL4_CapInitThreadCNode, cnode_depth_, right_slot, 1);
        if (second != seL4_NoError) {
            /* The parent held one child, not two: a retype already spent its
             * low half, so the parent is partial. It must leave the list -- a
             * partial parent left listed was the bug, retried forever -- and
             * the child that did succeed is a whole piece, so it is kept. The
             * rest of the parent is lost; recovering it needs the kernel to say
             * how much is left, which it does not. */
            unlink(parent);
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
        /* The parent leaves the free list but its node stays: it is the merge
         * anchor, and its memory is reclaimable once both children are deleted
         * (the kernel resets a childless untyped's free index on the next
         * retype, kernel/src/object/untyped.c:184-189). */
        unlink(parent);
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
    return node;
}

void Allocator::free_piece(Node *node) noexcept
{
    /* A piece whose buddy is free merges with it: delete both children (so the
     * parent has none and its memory resets), return their node slots, and give
     * the parent back in turn (allocman's `_utspace_split_free`). */
    if (node->parent != nullptr && node->sibling != nullptr && node->sibling->free != 0) {
        Node *const sibling = node->sibling;
        Node *const parent = node->parent;
        unlink(sibling);
        seL4_CNode_Delete(seL4_CapInitThreadCNode, node->cap, cnode_depth_);
        seL4_CNode_Delete(seL4_CapInitThreadCNode, sibling->cap, cnode_depth_);
        free_node(sibling);
        free_node(node);
        free_piece(parent);
    } else {
        insert(node->device != 0, node);
    }
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
                                 seL4_CapInitThreadCNode, cnode_depth_, slot, 1);
    if (*error != seL4_NoError) {
        slot_failed(slot);
        insert(false, node);
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
                                    seL4_CapInitThreadCNode, cnode_depth_, slot, 1);
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

void Allocator::adopt_slots(seL4_CPtr first, seL4_Word count, seL4_Word depth) noexcept
{
    slots_first_ = first;
    slots_next_ = first;
    slots_end_ = first + count;
    slots_used_ = 0;
    cnode_depth_ = depth;
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
    /* What is handed out is a piece the splitting made, so it has nothing
     * derived from it and the kernel will let the caller copy it -- giving it
     * to a service is the point (specs/authority.md), and a capability with
     * derived objects cannot be copied ("RevokeFirst: The untyped has been used
     * to retype an object",
     * out/aegir/libsel4/include/interfaces/sel4_client.h:419). */
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
                            seL4_CapInitThreadCNode, cnode_depth_, slot, 1);
    if (retyped != seL4_NoError) {
        slot_failed(slot);
        *error = retyped;
        return 0;
    }
    account.objects += 1;
    return slot;
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
