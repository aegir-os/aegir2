/*
 * Resolving a host name: Sys:S/hosts first, then the stack's DNS
 * (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The resolver belongs to the *client*, not the stack: it reads the filesystem
 * itself, so a name the hosts file holds resolves with no network at all -- the
 * offline case the acceptance and a board both need -- and only a name the file
 * lacks goes to the DHCP-supplied DNS server through the socket port. The stack
 * never reads a filesystem.
 *
 * Both ports are found through the bootstrap block: a client says in its
 * manifest `needs` that it uses vfs.namespace and net.socket, and the library
 * finds them by name. A client without the namespace resolves by DNS alone; one
 * without the socket port resolves by the hosts file alone.
 */

#ifndef AEGIR_RESOLVE_H
#define AEGIR_RESOLVE_H

#include <stdint.h>

namespace aegir::resolve {

/** Resolve `name` to an IPv4 address in network order. True when a table or the
 *  DNS server knew it. `name` is not NUL-terminated; `length` is its own. */
bool lookup(char const *name, uint32_t length, uint32_t *address) noexcept;

/** A dotted-quad "a.b.c.d" into the network-order word the socket port and the
 *  hosts file both use, or 0 when it is not an address. Exposed because a
 *  caller (ping) sorts an argument into "address or name" before resolving. */
uint32_t parse_ipv4(char const *text) noexcept;

/** Consider one hosts-file line -- `address name [aliases...]` -- against a
 *  name. True, with `*address` set, when a name matches; `#` starts a comment,
 *  blank lines are nothing, and anything that is not an address first is
 *  ignored. Exposed so the hosted resolver, which streams the file through the
 *  runtime's own file layer, matches names by the same grammar this one does. */
bool hosts_line(char const *line, uint32_t length, char const *name,
                uint32_t name_length, uint32_t *address) noexcept;

}  // namespace aegir::resolve

#endif  // AEGIR_RESOLVE_H
