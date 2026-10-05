/*
 * The runtime's socket calls: musl's socket(2) family, answered from the net
 * stack's socket port (specs/net.md, specs/cxx.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A hosted program uses `socket`, `connect`, `send`, `recv`, `getaddrinfo` and
 * never touches a `tcp_pcb` -- nor seL4. The syscalls musl issues reach the
 * heap's dispatcher (heap.cc), which answers them here the way it answers the
 * filesystem calls in files.cc: an Aegir resource behind a POSIX shape. The
 * one resource is the socket port (aegir/net.h), found by name through the
 * bootstrap block like the namespace; a process the session did not give
 * `net.socket` has no sockets and every call is refused with -ENOSYS or -EIO.
 *
 * A socket fd is a row in this layer's own table -- an fd-to-connection table,
 * as the spec calls it -- not a file. The two fd spaces are kept apart by
 * range: a file fd is small (files.cc grows from 3), a socket fd starts at
 * kSocketFdBase, and the dispatcher routes a call by which range its fd is in.
 * That is a routing marker, not a capacity: the socket table grows on demand
 * the way the file table does.
 *
 * Plain types only, so heap.cc can include this without an seL4 header of its
 * own; the free functions are named after the syscall they answer, not the
 * libc function, because that is the layer they intercept.
 */

#ifndef AEGIR_NETWORK_H
#define AEGIR_NETWORK_H

#include <stddef.h>
#include <stdint.h>

namespace aegir::network {

/** The first fd a socket gets. Everything below is a file (files.cc). */
constexpr int kSocketFdBase = 1 << 20;

/** Find the socket port. Called at startup (heap::init); before it, every
 *  socket call is refused. */
void adopt() noexcept;

/** Whether `fd` is this layer's (its range, and a live row). The dispatcher's
 *  routing test for close, read and write. */
bool owns(int fd) noexcept;

/* One function per socket syscall musl issues, with the arguments it passes
 * the kernel. Each answers a value or a negative errno the way a syscall
 * does. `address` is a `struct sockaddr_in` (AF_INET). */
long socket(int domain, int type, int protocol) noexcept;
long bind(int fd, void const *address, int address_length) noexcept;
long listen(int fd, int backlog) noexcept;
long accept(int fd, void *address, int *address_length) noexcept;
long connect(int fd, void const *address, int address_length) noexcept;
long sendto(int fd, void const *buffer, size_t length, int flags, void const *address,
            int address_length) noexcept;
long recvfrom(int fd, void *buffer, size_t length, int flags, void *address,
              int *address_length) noexcept;
long shutdown(int fd, int how) noexcept;
long getsockname(int fd, void *address, int *address_length) noexcept;
long getpeername(int fd, void *address, int *address_length) noexcept;
long setsockopt(int fd, int level, int name, void const *value, int length) noexcept;
long getsockopt(int fd, int level, int name, void *value, int *length) noexcept;
long close(int fd) noexcept;

/** Resolve a name -- a dotted quad or a host name -- to an address (network
 *  order), or 0. The shim's `getaddrinfo`/`gethostbyname` stand on this, and
 *  so does a `connect` given a name's address already. `timeout` is in
 *  milliseconds, 0 to wait. */
uint32_t resolve(char const *name, size_t length, uint32_t timeout) noexcept;

}  // namespace aegir::network

#endif  // AEGIR_NETWORK_H
