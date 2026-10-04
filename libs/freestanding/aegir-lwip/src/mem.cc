/*
 * lwIP's heap for Aegir: a page-backed free-list allocator, wired in through
 * MEM_CUSTOM_ALLOCATOR so lwIP's own mem.c calls it.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * lwIP's stock heap is a fixed `MEM_SIZE`; this one has no size at all. When a
 * block large enough is not free, the heap asks its **source** for another run
 * of mapped memory and adds the run to the free list. The source is the
 * service's (aegir/lwip/port.h): it retypes the run out of the untyped the
 * stack was given and maps it, so the heap is as big as what the stack holds
 * and grows on demand rather than from a build constant (specs/net.md,
 * AGENTS.md's rule against fixed carves).
 *
 * The free list is kept sorted by address, so a block freed next to another
 * merges with it -- including across two runs that happen to be adjacent, which
 * is why the merge is by address rather than per region. Each block carries its
 * size in a header before the payload; the payload is 16-byte aligned, which is
 * enough for anything lwIP puts there. It is deliberately simple: lwIP's
 * allocations are small and long-lived, so readable beats clever.
 */

#include <aegir/lwip/port.h>

#include <stddef.h>
#include <stdint.h>

namespace {

constexpr uint32_t kAlign = 16;
/* The header before each payload, sized so payloads stay aligned. Only the
 * first two words are used; the rest is the alignment's price. */
constexpr uint32_t kHeader = 16;
/* The smallest split worth making: a whole header plus one aligned payload. */
constexpr uint32_t kMinSplit = kHeader + kAlign;

struct Block {
    uint32_t size; /* total bytes of this block, header included */
    uint32_t free;
    Block *next; /* the free list, when free */
    Block *prev; /* the free list, when free */
};

Block *g_head = nullptr; /* free list, sorted by address */
aegir::lwip::HeapSource g_source = nullptr;
void *g_source_context = nullptr;

uint32_t align_up(uint32_t bytes) noexcept
{
    return (bytes + kAlign - 1u) & ~(kAlign - 1u);
}

/* Put [block, block + size) on the free list, merging with the free blocks that
 * touch it. Adjacency is by address, so a block from one run merges with a
 * neighbour from the same run or from a run the source handed us next to it. */
void insert_free(Block *block, uint32_t size) noexcept
{
    block->size = size;
    block->free = 1;
    Block *link = g_head;
    Block *previous = nullptr;
    while (link != nullptr && link < block) {
        previous = link;
        link = link->next;
    }
    block->prev = previous;
    block->next = link;
    if (previous != nullptr) {
        previous->next = block;
    } else {
        g_head = block;
    }
    if (link != nullptr) {
        link->prev = block;
    }
    /* Merge with the following block. */
    if (block->next != nullptr &&
        reinterpret_cast<uint8_t *>(block) + block->size ==
            reinterpret_cast<uint8_t *>(block->next)) {
        Block *const following = block->next;
        block->size += following->size;
        block->next = following->next;
        if (following->next != nullptr) {
            following->next->prev = block;
        }
    }
    /* And with the one before. */
    if (block->prev != nullptr &&
        reinterpret_cast<uint8_t *>(block->prev) + block->prev->size ==
            reinterpret_cast<uint8_t *>(block)) {
        Block *const before = block->prev;
        before->size += block->size;
        before->next = block->next;
        if (block->next != nullptr) {
            block->next->prev = before;
        }
    }
}

/* Take a block of at least `need` from the free list, splitting it when the
 * remainder is worth keeping. Null when nothing is big enough. */
Block *take(uint32_t need) noexcept
{
    for (Block *block = g_head; block != nullptr; block = block->next) {
        if (block->size < need) {
            continue;
        }
        /* Unlink first: a split's remainder sits right after the block, and
         * inserting it while the block is still on the list would merge it
         * straight back. */
        if (block->prev != nullptr) {
            block->prev->next = block->next;
        } else {
            g_head = block->next;
        }
        if (block->next != nullptr) {
            block->next->prev = block->prev;
        }
        if (block->size - need >= kMinSplit) {
            auto *const rest =
                reinterpret_cast<Block *>(reinterpret_cast<uint8_t *>(block) + need);
            insert_free(rest, block->size - need);
        }
        block->size = need;
        block->free = 0;
        block->next = nullptr;
        block->prev = nullptr;
        return block;
    }
    return nullptr;
}

}  // namespace

namespace aegir::lwip {

void set_heap_source(HeapSource source, void *context) noexcept
{
    g_source = source;
    g_source_context = context;
}

}  // namespace aegir::lwip

extern "C" void *aegir_lwip_malloc(size_t bytes)
{
    if (bytes == 0) {
        return nullptr;
    }
    uint32_t const need = align_up(static_cast<uint32_t>(bytes)) + kHeader;
    Block *block = take(need);
    if (block == nullptr) {
        if (g_source == nullptr) {
            return nullptr; /* no heap and no way to get one: refuse */
        }
        uint32_t got = 0;
        void *const region = g_source(g_source_context, need, &got);
        if (region == nullptr || got < need) {
            return nullptr;
        }
        insert_free(reinterpret_cast<Block *>(region), got);
        block = take(need);
    }
    return block != nullptr ? reinterpret_cast<uint8_t *>(block) + kHeader : nullptr;
}

extern "C" void aegir_lwip_free(void *pointer)
{
    if (pointer == nullptr) {
        return;
    }
    auto *const block =
        reinterpret_cast<Block *>(static_cast<uint8_t *>(pointer) - kHeader);
    insert_free(block, block->size);
}

extern "C" void *aegir_lwip_calloc(size_t count, size_t bytes)
{
    size_t const total = count * bytes;
    void *const result = aegir_lwip_malloc(total);
    if (result != nullptr) {
        auto *const out = static_cast<volatile uint8_t *>(result);
        for (size_t i = 0; i < total; ++i) {
            out[i] = 0;
        }
    }
    return result;
}
