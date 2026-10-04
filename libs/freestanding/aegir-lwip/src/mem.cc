/*
 * lwIP's heap for Aegir: a free-list allocator over the region the service was
 * given, wired in through MEM_CUSTOM_ALLOCATOR so lwIP's own mem.c calls it.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * lwIP's stock heap is a fixed `MEM_SIZE`; this one is as big as the region the
 * service holds, so the stack's memory grows from what it was given rather than
 * from a build constant (specs/net.md). A block carries its size in a header
 * before the payload, and blocks are walked from the base -- a first fit with a
 * forward coalesce on free. The region is page-aligned and every block is a
 * multiple of the alignment, so a payload is aligned for any type lwIP puts
 * there.
 *
 * It is deliberately simple: lwIP's allocations are small and long-lived, the
 * region is not huge, and an allocator nobody can read is worse than one that
 * is a little slow.
 */

#include <aegir/lwip/port.h>

#include <stddef.h>
#include <stdint.h>

namespace {

constexpr uint32_t kAlign = 16;
/* The header before each payload, sized so payloads stay aligned. Only the
 * first two words are used; the padding is the alignment's price. */
constexpr uint32_t kHeader = 16;

struct Block {
    uint32_t size; /* total bytes of this block, header included */
    uint32_t free;
};

uint8_t *g_base = nullptr;
uint8_t *g_end = nullptr;

uint32_t align_up(uint32_t bytes) noexcept
{
    return (bytes + kAlign - 1u) & ~(kAlign - 1u);
}

}  // namespace

namespace aegir::lwip {

void set_heap(void *base, uint32_t size) noexcept
{
    g_base = static_cast<uint8_t *>(base);
    g_end = g_base != nullptr && size >= kHeader ? g_base + size : nullptr;
    if (g_end != nullptr) {
        auto *const whole = reinterpret_cast<Block *>(g_base);
        whole->size = static_cast<uint32_t>(g_end - g_base);
        whole->free = 1;
    }
}

}  // namespace aegir::lwip

extern "C" void *aegir_lwip_malloc(size_t bytes)
{
    if (g_base == nullptr) {
        return nullptr;
    }
    uint32_t const need = align_up(static_cast<uint32_t>(bytes)) + kHeader;
    uint8_t *p = g_base;
    while (p + kHeader <= g_end) {
        auto *const block = reinterpret_cast<Block *>(p);
        if (block->size < kHeader) {
            break; /* corrupt: stop rather than walk forever */
        }
        if (block->free != 0 && block->size >= need) {
            uint32_t const rest = block->size - need;
            if (rest >= kHeader + kAlign) {
                block->size = need;
                auto *const next = reinterpret_cast<Block *>(p + need);
                next->size = rest;
                next->free = 1;
            }
            block->free = 0;
            return p + kHeader;
        }
        p += block->size;
    }
    return nullptr;
}

extern "C" void aegir_lwip_free(void *pointer)
{
    if (pointer == nullptr || g_base == nullptr) {
        return;
    }
    uint8_t *p = static_cast<uint8_t *>(pointer) - kHeader;
    reinterpret_cast<Block *>(p)->free = 1;
    /* Coalesce forward: join the next block while it is free. */
    for (;;) {
        auto *const block = reinterpret_cast<Block *>(p);
        uint8_t *const next = p + block->size;
        if (next + kHeader > g_end) {
            break;
        }
        auto *const following = reinterpret_cast<Block *>(next);
        if (following->free == 0 || following->size < kHeader) {
            break;
        }
        block->size += following->size;
    }
}

extern "C" void *aegir_lwip_calloc(size_t count, size_t bytes)
{
    size_t const total = count * bytes;
    void *const result = aegir_lwip_malloc(total);
    if (result != nullptr) {
        auto *const bytes_out = static_cast<volatile uint8_t *>(result);
        for (size_t i = 0; i < total; ++i) {
            bytes_out[i] = 0;
        }
    }
    return result;
}
