/*
 * aegir-net's link half: a netif and a receive thread for each bound Ethernet
 * link, and the control the stack's control port gives over them
 * (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The stack knows nothing about virtio: it learns of links through the device
 * manager's registry (each bound `eth.*` row), opens the link port, maps the
 * window the port serves through, reads the MAC, MTU and link state from the
 * link's own `info`, and adds a netif. A dedicated thread per link calls
 * `receive` (a held reply) in a loop and hands each frame to lwIP through the
 * netif's input; `linkoutput` copies an outgoing frame into the window and
 * calls `send`. Nothing here names a machine.
 */

#ifndef AEGIR_NET_INTERFACE_H
#define AEGIR_NET_INTERFACE_H

#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/thread.h>
#include <stdint.h>

namespace aegir::net {

/** What the link half needs from the service: the allocator its per-link state
 *  comes out of, the window it maps link windows through, and the thread
 *  builder and placement a receive thread runs at. */
struct Authority {
    mem::Allocator &objects;
    mem::Scratch &scratch;
    mem::Account &account;
    thread::Builder &builder;
    thread::Placement const &placement;
};

/** Add a netif, and start a receive thread, for every bound `eth.*` link the
 *  registry knows. Each starts **down** -- only a configuration brings it up
 *  (specs/net.md) -- with the device's own MAC, MTU and link state. Returns how
 *  many interfaces were added. */
unsigned add_links(Authority const &authority) noexcept;

/** How many adapters the stack serves. */
unsigned link_count() noexcept;

/** One adapter, as the control port describes it (aegir/netcontrol.h): its name
 *  packed low-byte-first into `name`, then its IPv4 address, netmask and
 *  gateway (network order), then its state bits. False when `index` is past the
 *  count. The addresses are what the stack last learned -- DHCP's answer, or a
 *  static `set` -- cached when lwIP reported them, so a reader does not race
 *  the tcpip thread. */
struct LinkState {
    uint64_t name; /* up to 8 ASCII bytes, low byte first */
    uint64_t ipv4;
    uint64_t netmask;
    uint64_t gateway;
    uint64_t flags;
};
bool link_state(unsigned index, LinkState *state) noexcept;

/** Apply a `set` (aegir/netcontrol.h) to adapter `index`: the parameter and its
 *  value. The change runs in lwIP's tcpip thread -- where every netif operation
 *  belongs -- and the call waits for it. False when the index or the parameter
 *  is not one the stack speaks. */
bool link_configure(unsigned index, uint32_t parameter, uint64_t value) noexcept;

}  // namespace aegir::net

#endif  // AEGIR_NET_INTERFACE_H
