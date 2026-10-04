/*
 * virtio-net's device shape: its config space, its features, and the header a
 * frame carries in a queue (virtio 1.x, 5.1).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The driver reads the config space to learn the MAC, the link state and the
 * MTU -- none of them compiled in, because a board's NIC answers differently
 * (specs/net.md). The `device id` is 1 (mmio.h's kDeviceIdNet).
 */

#pragma once

#include <stdint.h>

namespace aegir::virtio::net {

/* The config window, as byte offsets from kConfig: the device's MAC, then its
 * link status, then the queue-pair count, then (with VIRTIO_NET_F_MTU) the
 * MTU. Byte-granular because the MAC is six bytes and the status sits at
 * offset 6 (virtio 1.x, 5.1.4). */
constexpr uint32_t kConfigMac = 0;
constexpr uint32_t kConfigStatus = 6;
constexpr uint32_t kConfigMaxVirtqueuePairs = 8;
constexpr uint32_t kConfigMtu = 10;

/* Feature bits, low word. Only these three are asked for: the MAC (so the
 * stack has a hardware address to use), the link status (so it knows the link
 * is up), and the MTU when the device offers one. No offload is negotiated --
 * checksums are the guest's, the portable choice (specs/net.md). */
constexpr uint32_t kFeatureMtu = 1u << 3;
constexpr uint32_t kFeatureMac = 1u << 5;
constexpr uint32_t kFeatureStatus = 1u << 16;

/** The link is up, read from the config space's status (VIRTIO_NET_S_LINK_UP). */
constexpr uint16_t kStatusLinkUp = 1;

/** The header the device prepends to every frame in a queue. It is the 10-byte
 *  virtio_net_hdr: we do not negotiate MRG_RXBUF, so there is no trailing
 *  `num_buffers` field, and no GSO/csum, so the header is ignored on receive
 *  and zeroed on send. */
constexpr uint32_t kHeaderBytes = 10;

/** The two queues: receive first, transmit second (virtio 1.x, 5.1.2). */
constexpr uint32_t kReceiveQueue = 0;
constexpr uint32_t kTransmitQueue = 1;

/** The most bytes one posted receive buffer must hold: the header plus a full
 *  frame. The driver rounds this up to a stride so buffers are aligned. */
constexpr uint32_t kReceiveBufferBytes = kHeaderBytes + 1518;

}  // namespace aegir::virtio::net
