/*
 * The network service's side of lwIP's port: how lwIP gets its memory and its
 * threading, without a fixed size anywhere (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * lwIP's stock allocator is a fixed region (`MEM_SIZE`), which is a carve by
 * another name. So its `mem_malloc`/`mem_free` are routed to Aegir's own
 * (`MEM_CUSTOM_ALLOCATOR`), and that allocator asks a **source** for another run
 * of mapped memory whenever it has none big enough -- so the heap grows from the
 * untyped the service holds (and later `mem.main`) rather than from a build
 * constant. The service calls `set_heap_source` once, before lwIP starts.
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

/** Where lwIP's heap gets more room: hand back a **contiguous, mapped** region
 *  of at least `bytes`, its real size through `got_bytes` (rounded up -- a page
 *  granularity is fine), or null when there is none. The service's source
 *  retypes the region out of the untyped it was given and maps it into its own
 *  window, so the heap is the service's memory and not a number in a header. */
using HeapSource = void *(*)(void *context, uint32_t bytes, uint32_t *got_bytes);

/** Set the heap's source, once, before lwIP starts. A heap with no source
 *  refuses every allocation rather than inventing one. */
void set_heap_source(HeapSource source, void *context) noexcept;

/** Where the sys_arch retypes its notifications from: the service's allocator
 *  over its untyped, and the account its objects are charged to. Called once,
 *  before lwIP starts. */
void set_objects(aegir::mem::Allocator &objects, aegir::mem::Account &account) noexcept;

/** How the sys_arch starts a thread (lwIP's `sys_thread_new`): the service's
 *  thread builder and the placement the tcpip thread runs at. Called once. */
void set_threading(aegir::thread::Builder &builder,
                   aegir::thread::Placement const &placement) noexcept;

/** The TCB of the thread `sys_thread_new` started (the tcpip thread), or 0
 *  before one exists. The service binds the timer-tick notification to it, so
 *  the tcpip thread's timed wait sees the tick. */
seL4_CPtr thread_tcb() noexcept;

/** Advance the cached monotonic millisecond count that `sys_now` answers with.
 *  The service calls it from its tick, so `sys_now` is not a port call per
 *  read. */
void set_now(uint32_t milliseconds) noexcept;

/** The tick's period in milliseconds. The sys_arch adds it to the cached
 *  `sys_now` each time the timer tick wakes the tcpip thread, so lwIP's timers
 *  -- which run on the tcpip thread and read `sys_now` -- actually advance. A
 *  `sys_now` that never moves makes every timeout never fire (specs/net.md). */
void set_tick_milliseconds(uint32_t milliseconds) noexcept;

}  // namespace aegir::lwip

#endif  // AEGIR_LWIP_PORT_H
