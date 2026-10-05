/*
 * The socket port's protocol: Aegir's BSD-shaped client API (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the same shape as aegir/ethernet.h and aegir/block.h:
 * the stack includes it to serve, a client includes it to call. It is shaped
 * the way `socket(2)` is, so a client -- including a hosted one through the
 * later musl shim -- uses familiar calls and never touches a `tcp_pcb`.
 *
 * The first slice is a **raw ICMP socket**, which is what `ping` needs: a
 * client creates one, sends an echo request, and receives until the reply
 * comes. The protocol grows by adding family/type/protocol combinations and
 * the methods they need, not by guessing at TCP before it is exercised. The
 * calls that genuinely wait -- `recv`, `resolve` -- are **held replies**: the
 * caller blocks inside the call until the answer exists (specs/signal.md).
 */

#ifndef AEGIR_NET_H
#define AEGIR_NET_H

#include <stdint.h>

namespace aegir::net {

constexpr char kPortName[] = "net.socket";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

/* socket(2)'s arguments, as the values the C library uses. The first slice
 * speaks one combination -- AF_INET / SOCK_RAW / IPPROTO_ICMP -- and refuses
 * the rest. */
constexpr uint32_t kAfInet = 2;
constexpr uint32_t kSockRaw = 3;
constexpr uint32_t kIpprotoIcmp = 1;

/* socket: three request words (domain, type, protocol). The answer is one word,
 * the socket's id, or 0 when the combination is one the stack does not speak. */
constexpr uint32_t kMethodSocket = 1;
constexpr uint32_t kSocketWords = 1;

/* close: one request word, the id. The answer is one word, 1 when it is gone. */
constexpr uint32_t kMethodClose = 2;

/* send: the id, the destination address (network order), the payload's length
 * in bytes, then the payload words (low byte first). For a raw socket the
 * payload is the whole IP payload -- the ICMP message -- and the stack adds the
 * IP header. The answer is the payload bytes the stack took. */
constexpr uint32_t kMethodSend = 3;

/* recv: the id and a timeout in milliseconds (0 waits forever). The answer is
 * *held* until a reply arrives or the timeout passes, then the source address
 * (network order), the payload's length -- zero on timeout, which is also
 * end-of-stream -- and the payload words. */
constexpr uint32_t kMethodRecv = 4;

/* resolve: the name's length, a timeout in milliseconds (0 waits forever), then
 * the name's bytes as words, low byte first, no NUL (a name is at most
 * kMaxNameBytes). The answer is *held* until the name resolves or the timeout
 * passes, then one word, the address (network order), or 0 when it does not
 * resolve. */
constexpr uint32_t kMethodResolve = 5;

/* A name's ceiling: DNS's own limit, not a number of ours. */
constexpr uint32_t kMaxNameBytes = 255;

/* The payload ceiling the message registers allow: the kernel's message length
 * minus the method, the protocol's own words, and a little slack. An ICMP
 * message is far below it; this is the bound a caller is refused at. */
constexpr uint32_t kMaxPayloadWords = 108;

}  // namespace aegir::net

#endif  // AEGIR_NET_H
