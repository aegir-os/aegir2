/*
 * The network stack's control port: how an adapter is listed and configured,
 * apart from the socket port (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the same shape as aegir/ethernet.h. It is deliberately
 * *not* aegir/net.h (the socket API): moving bytes and reprogramming an
 * interface are different questions, so they are different ports. The boot
 * manifest's reader and the live `Net:` volume are both clients of this one;
 * an interface starts **down**, and only a `set` here brings it up.
 *
 * The adapter is named by index -- the order `list` answers in -- so a client
 * learns the names `describe` gives back rather than assuming one.
 */

#ifndef AEGIR_NETCONTROL_H
#define AEGIR_NETCONTROL_H

#include <stdint.h>

namespace aegir::netcontrol {

constexpr char kPortName[] = "net.control";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

/** List: no request words. The answer is one word, the number of adapters. An
 *  index in `describe`/`set` is below this. */
constexpr uint32_t kMethodList = 1;
constexpr uint32_t kListWords = 1;

/** Describe: one request word, the adapter's index. The answer is four words:
 *  the adapter's name (up to 8 ASCII bytes, low byte first, the MAC's packing),
 *  then its IPv4 address, netmask and gateway (network order, zero when unset),
 *  and a state word of `kState*` bits. */
constexpr uint32_t kMethodDescribe = 2;
constexpr uint32_t kDescribeWords = 5;

/** Set: three request words -- the adapter's index, a `kParam*` parameter, and
 *  its value. The answer is one word, 1 when the parameter was taken. A `set`
 *  is the one act that changes an interface, and the value's meaning is the
 *  parameter's. */
constexpr uint32_t kMethodSet = 3;
constexpr uint32_t kSetWords = 3;

/** Set-text: a `set` whose value is text rather than a word. Request words: the
 *  adapter's index, the parameter, the value's byte length, then the bytes
 *  packed low byte first (the packing `aegir/net.h`'s DNS name uses). The
 *  answer is one word, 1 when the parameter was taken. The text's ceiling is
 *  the message envelope's, not a number chosen here. */
constexpr uint32_t kMethodSetText = 4;

/** Get-text: read a text parameter. Request words: the adapter's index and the
 *  parameter. The answer is the byte length followed by the bytes packed low
 *  byte first; a single zero word means the parameter is not one the stack
 *  speaks or has no value. */
constexpr uint32_t kMethodGetText = 5;

/* Parameters (the `set` request's second word). */
/** DHCP: 1 asks the network for an address, netmask, gateway and DNS server and
 *  brings the interface up; 0 stops it. The numbers are the network's, never
 *  the build's (specs/net.md). */
constexpr uint32_t kParamDhcp = 1;
/** Up: 1 brings the interface up with its current (or unset) addresses; 0 takes
 *  it down. */
constexpr uint32_t kParamUp = 2;
/** The static addresses, network order, for an interface configured by hand
 *  rather than by DHCP (specs/net.md). */
constexpr uint32_t kParamIpv4Address = 3;
constexpr uint32_t kParamIpv4Netmask = 4;
constexpr uint32_t kParamIpv4Gateway = 5;
/** Hostname: the machine's name, text (`kMethodSetText`/`kMethodGetText`). The
 *  stack puts it in DHCP option 12, so it is what the network sees and not only
 *  a label the machine keeps (specs/net.md). */
constexpr uint32_t kParamHostname = 6;

/* State bits (the `describe` answer's last word). */
constexpr uint64_t kStateUp = 1;     /* the interface is up */
constexpr uint64_t kStateDhcp = 2;   /* DHCP is enabled on it */
constexpr uint64_t kStateLinkUp = 4; /* the link's device says the link is up */

}  // namespace aegir::netcontrol

#endif  // AEGIR_NETCONTROL_H
