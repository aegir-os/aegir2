/*
 * aegir-net's socket half: the BSD-shaped client port (aegir/net.h,
 * specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The first slice is a raw ICMP socket, which is what ping needs: a client
 * creates one, sends an echo request, and receives the reply. The waiting calls
 * (`recv`, `resolve`) are held replies: their caller blocks inside the call and
 * is answered later -- `recv` from the link's receive path, running in the
 * tcpip thread, and `resolve` from DNS. Every thread shares the process's
 * CSpace, so the tcpip thread answers a reply cap the serve thread saved.
 */

#ifndef AEGIR_NET_SOCKET_H
#define AEGIR_NET_SOCKET_H

#include <aegir/thread.h>

namespace aegir::net {

/** Serve the socket port on this thread. Never returns. Takes the same
 *  authority the link half does (it is started with it), of which it uses the
 *  allocator for its reply slots and lwIP's heap for its state. */
[[noreturn]] void serve_sockets(void *authority) noexcept;

}  // namespace aegir::net

#endif  // AEGIR_NET_SOCKET_H
