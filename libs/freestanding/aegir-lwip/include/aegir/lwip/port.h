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

#include <aegir/mem/allocator.h>
#include <aegir/thread.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::lwip {

/** The badge bit the service mints its timer-tick notification with, so the
 *  tcpip thread's timed wait can tell a tick from a mailbox wake.
 *  `aegir::signal`'s context convention; high, so it clears any caller badge. */
constexpr seL4_Word kTickBit = 1ull << 31;

/** Give lwIP its heap: the region `base`..`base + size`, which the service holds
 *  (a frame mapped from its untyped, or a piece it carved). Called once, before
 *  lwIP starts. An allocator with no heap refuses every allocation rather than
 *  inventing one. */
void set_heap(void *base, uint32_t size) noexcept;

/** Where the sys_arch retypes its notifications from: the service's allocator
 *  over its untyped, and the account its objects are charged to. Called once,
 *  before lwIP starts. */
void set_objects(aegir::mem::Allocator &objects, aegir::mem::Account &account) noexcept;

/** How the sys_arch starts a thread (lwIP's `sys_thread_new`): the service's
 *  thread builder and the placement the tcpip thread runs at. Called once. */
void set_threading(aegir::thread::Builder &builder,
                   aegir::thread::Placement const &placement) noexcept;

/** Advance the cached monotonic millisecond count that `sys_now` answers with.
 *  The service calls it from its tick, so `sys_now` is not a port call per
 *  read. */
void set_now(uint32_t milliseconds) noexcept;

}  // namespace aegir::lwip

#endif  // AEGIR_LWIP_PORT_H
