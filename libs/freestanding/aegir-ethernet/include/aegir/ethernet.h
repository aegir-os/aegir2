/*
 * The Ethernet link port's protocol: what a network device driver serves, and
 * what the stack calls (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the same shape as aegir/block.h: the driver includes it
 * to serve, the client (the stack) includes it to call, and neither has to
 * guess what the other meant. It is the seam that keeps the stack free of any
 * machine: the stack sees Ethernet frames and a MAC, never a virtio transport,
 * a PCI function or a board's register map. A board's NIC is a new driver
 * behind this same port and nothing in the stack changes.
 *
 * Frames do not fit the envelope (a full frame is 1518 bytes; the message
 * registers carry 952), so the data half of the protocol travels through the
 * client's shared window, the way block data does; the words carry lengths and
 * the method. This header declares `info` now; `send` and `receive` join it
 * with the window.
 */

#ifndef AEGIR_ETHERNET_H
#define AEGIR_ETHERNET_H

#include <stdint.h>

namespace aegir::ethernet {

/** Info: no request words. The answer is three words -- the device's 6-byte
 *  MAC address in the low 48 bits of the first (byte 0 in the lowest 8 bits),
 *  the MTU, and flags (bit 0: the link is up). The stack sets its netif's
 *  hardware address and MTU from this, so nothing about a machine is compiled
 *  into the stack (specs/net.md). */
constexpr uint32_t kMethodInfo = 1;
constexpr uint32_t kInfoWords = 3;
constexpr uint64_t kInfoLinkUp = 1;

/** The frame ceiling: an Ethernet frame with an 802.1Q tag and no FCS. The
 *  window a client provides must hold one. */
constexpr uint32_t kFrameMax = 1518;

/** The MTU a device that does not report one is assumed to have -- the
 *  protocol's own number, not a machine's. */
constexpr uint32_t kMtuDefault = 1500;

/** The MAC address is six bytes, packed into the low 48 bits of a word. */
constexpr uint32_t kMacBytes = 6;

}  // namespace aegir::ethernet

#endif  // AEGIR_ETHERNET_H
