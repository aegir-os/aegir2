/*
 * The network service's side of lwIP's port: how lwIP gets its memory and its
 * threading, without a fixed size anywhere (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * lwIP's stock allocator is a fixed region (`MEM_SIZE`), which is a carve by
 * another name. So its `mem_malloc`/`mem_free` are routed to Aegir's own
 * (`MEM_CUSTOM_ALLOCATOR`), and that allocator hands out pieces of the region
 * the service was given -- the same untyped its notifications and its thread
 * come from, and later `mem.main`. The service calls `set_heap` once, before
 * lwIP starts, with the region it owns.
 */

#ifndef AEGIR_LWIP_PORT_H
#define AEGIR_LWIP_PORT_H

#include <stdint.h>

namespace aegir::lwip {

/** Give lwIP its heap: the region `base`..`base + size`, which the service holds
 *  (a frame mapped from its untyped, or a piece it carved). Called once, before
 *  lwIP starts. An allocator with no heap refuses every allocation rather than
 *  inventing one. */
void set_heap(void *base, uint32_t size) noexcept;

}  // namespace aegir::lwip

#endif  // AEGIR_LWIP_PORT_H
