/*
 * The heap's free regions (specs/memory.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * An `mmap` grows the heap's region down and maps frames at the addresses it
 * hands out; an `munmap` releases a span. What releases it must be something
 * the next `mmap` can hand back, or every large allocation a program frees
 * leaks its frames for the life of the process -- which is what an `munmap`
 * that does nothing costs.
 *
 * The list of released regions is kept here, in **nodes the caller owns** --
 * mapped pages the heap cuts up, the way aegir-mem's allocator keeps its own
 * pool -- and not in the regions themselves. That is the lesson of the first
 * attempt, which kept a two-word node inside each released region: the region
 * is not the heap's to describe, and a node written into it can be clobbered
 * after the release. The region's *contents* are not the caller's either: an
 * `mmap` promises zero-filled pages, so the heap zeroes a region before handing
 * it back out (specs/memory.md).
 *
 * Regions are address-ordered, so a release merges with an immediate neighbour
 * either side and a first fit finds the lowest region that holds the request.
 * Splitting hands out the region's top, which keeps the node where it is.
 *
 * The pool grows through a `NodeSource` the caller sets: the heap maps another
 * page below its cursor and hands it over, so growing the list never allocates
 * through the list it is growing. It is pure address arithmetic and a linked
 * list over caller-provided nodes, so the conformance (scripts/check_regions.py)
 * asserts it without a kernel.
 */

#ifndef AEGIR_HEAP_REGIONS_H
#define AEGIR_HEAP_REGIONS_H

#include <stddef.h>
#include <stdint.h>

namespace aegir::heap::detail {

class Regions {
public:
    /** Another node region: a mapped, writable run of at least `*bytes`, whose
     *  size is written back. Null when none can be had. */
    using NodeSource = void *(*)(void *context, unsigned *bytes);

    Regions() = default;
    Regions(Regions const &) = delete;
    Regions &operator=(Regions const &) = delete;

    /** The first node region. Mirrors the allocator's `adopt_nodes`. */
    void adopt(void *region, unsigned bytes) noexcept
    {
        if (region == nullptr || bytes < sizeof(Node)) {
            return;
        }
        Node *const nodes = static_cast<Node *>(region);
        unsigned const count = bytes / static_cast<unsigned>(sizeof(Node));
        for (unsigned i = 0; i < count; ++i) {
            nodes[i].next = free_nodes_;
            free_nodes_ = &nodes[i];
        }
    }

    /** Where another node region comes from when the pool runs out. */
    void set_node_source(NodeSource source, void *context) noexcept
    {
        node_source_ = source;
        node_context_ = context;
    }

    /** The lowest free region holding at least `bytes`, split so the region
     *  handed out is exactly `bytes`, or null when none does. The caller zeroes
     *  what it is given: `mmap` promises zero-filled pages. */
    void *take(uintptr_t bytes) noexcept
    {
        if (bytes == 0) {
            return nullptr;
        }
        Node **link = &head_;
        while (*link != nullptr) {
            Node *node = *link;
            if (node->bytes >= bytes) {
                uintptr_t const base = node->base;
                if (node->bytes == bytes) {
                    *link = node->next;
                    free_node(node);
                    return reinterpret_cast<void *>(base);
                }
                /* Hand out the top and leave the node where it is. */
                node->bytes -= bytes;
                return reinterpret_cast<void *>(base + node->bytes);
            }
            link = &node->next;
        }
        return nullptr;
    }

    /** Remember [base, base+bytes) as free, merging it with an immediate
     *  neighbour either side. False for a span that is empty, misaligned or
     *  overlaps a region already free -- a caller bug, refused rather than
     *  allowed to corrupt the list -- or when no node can be had. */
    bool give(uintptr_t base, uintptr_t bytes) noexcept
    {
        if (bytes == 0 || (base & (alignof(Node) - 1)) != 0) {
            return false;
        }
        Node **link = &head_;
        Node *before = nullptr;
        while (*link != nullptr && (*link)->base < base) {
            before = *link;
            link = &(*link)->next;
        }
        if (*link != nullptr && base + bytes > (*link)->base) {
            return false;
        }
        if (before != nullptr && before->base + before->bytes > base) {
            return false;
        }

        Node *node = alloc_node();
        if (node == nullptr) {
            return false;
        }
        node->base = base;
        node->bytes = bytes;
        node->next = *link;
        if (*link != nullptr && base + bytes == (*link)->base) {
            Node *const after = *link;
            node->bytes += after->bytes;
            node->next = after->next;
            free_node(after);
        }
        if (before != nullptr && before->base + before->bytes == base) {
            before->bytes += node->bytes;
            before->next = node->next;
            free_node(node);
        } else {
            *link = node;
        }
        return true;
    }

    bool empty() const noexcept { return head_ == nullptr; }

    /** How many regions are free, for a caller's report. */
    uint32_t count() const noexcept
    {
        uint32_t regions = 0;
        for (Node const *node = head_; node != nullptr; node = node->next) {
            ++regions;
        }
        return regions;
    }

    /** The total bytes free, for a caller's report. */
    uintptr_t bytes_free() const noexcept
    {
        uintptr_t total = 0;
        for (Node const *node = head_; node != nullptr; node = node->next) {
            total += node->bytes;
        }
        return total;
    }

private:
    struct Node {
        Node *next;
        uintptr_t base;
        uintptr_t bytes;
    };

    /* A node from the pool, growing it through the source when empty. Null when
     * there is none and no source, or the source refused. */
    Node *alloc_node() noexcept
    {
        if (free_nodes_ == nullptr && node_source_ != nullptr) {
            unsigned bytes = 0;
            void *const region = node_source_(node_context_, &bytes);
            adopt(region, bytes);
        }
        Node *node = free_nodes_;
        if (node != nullptr) {
            free_nodes_ = node->next;
        }
        return node;
    }

    void free_node(Node *node) noexcept
    {
        node->next = free_nodes_;
        free_nodes_ = node;
    }

    Node *head_ = nullptr;
    Node *free_nodes_ = nullptr;
    NodeSource node_source_ = nullptr;
    void *node_context_ = nullptr;
};

}  // namespace aegir::heap::detail

#endif  // AEGIR_HEAP_REGIONS_H
