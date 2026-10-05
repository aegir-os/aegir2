/*
 * The runtime's socket calls -- implementation. See network.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * musl's socket functions issue Linux syscalls; the heap's dispatcher answers
 * them here, in the process's own memory, by calling the net stack's socket
 * port (aegir/net.h) instead of the kernel. The pieces:
 *
 *   - the port is found by name (`net.socket`) through the bootstrap block,
 *     like the namespace in files.cc; a process not given it has no sockets.
 *   - the fd table is this layer's own and grows on demand: an open socket is
 *     the stack's id, the family and type it was made with, and -- for a UDP
 *     socket that `connect` has given a default peer -- that peer.
 *   - the sockaddr_in the app passes is decoded into the port's words (the
 *     address is already network order; the port is swapped to host order),
 *     and a recv's source is encoded back into the app's sockaddr.
 *
 * This is the seL4 side of the seL4/libc boundary (heap.cc's header says why),
 * so it includes aegir/net.h and no libc++ or musl header.
 */

#include <aegir/network.h>

#include <aegir/bootstrap.h>
#include <aegir/ipc/port.h>
#include <aegir/net.h>
#include <errno.h>
#include <stdlib.h>

namespace aegir::network {

namespace {

/* AF_INET and the socket types, as musl's <sys/socket.h> numbers them. */
constexpr int kAfInet = 2;
constexpr int kSockStream = 1;
constexpr int kSockDgram = 2;
constexpr int kSockRaw = 3;
constexpr int kIpprotoTcp = 6;
constexpr int kIpprotoUdp = 17;
constexpr int kIpprotoIcmp = 1;

/* The stack's payload ceiling, in words, plus the protocol's own words. */
constexpr uint32_t kAnswerWords = aegir::ipc::kMaxWords;

struct Entry {
    bool used;
    uint32_t id;     /* the stack's socket id */
    uint32_t family;
    uint32_t type;
    bool has_peer;   /* a UDP socket `connect` gave a default peer */
    uint32_t peer_address;
    uint32_t peer_port;
};

Entry *g_entries = nullptr;
uint32_t g_capacity = 0;
aegir::ipc::Consumer g_sockets{};

Entry *entry_for(int fd) noexcept
{
    if (fd < kSocketFdBase) {
        return nullptr;
    }
    uint32_t const row = static_cast<uint32_t>(fd - kSocketFdBase);
    if (row >= g_capacity || !g_entries[row].used) {
        return nullptr;
    }
    return &g_entries[row];
}

/* The smallest free row, growing the table on demand. Returns its fd, or -1. */
int alloc_fd() noexcept
{
    for (uint32_t row = 0;; ++row) {
        if (row >= g_capacity) {
            uint32_t capacity = g_capacity != 0 ? g_capacity : 16;
            while (capacity <= row) {
                capacity *= 2;
            }
            auto *grown = static_cast<Entry *>(realloc(g_entries, capacity * sizeof(Entry)));
            if (grown == nullptr) {
                return -1;
            }
            for (uint32_t j = g_capacity; j < capacity; ++j) {
                grown[j].used = false;
            }
            g_entries = grown;
            g_capacity = capacity;
        }
        if (!g_entries[row].used) {
            g_entries[row].used = true;
            return kSocketFdBase + static_cast<int>(row);
        }
    }
}

uint16_t swap16(uint16_t value) noexcept
{
    return static_cast<uint16_t>((value >> 8) | (value << 8));
}

/* A `struct sockaddr_in` as musl lays it out: family, port (network order),
 * address (network order), then zero padding to 16 bytes. */
constexpr uint32_t kSockaddrInBytes = 16;

/* Decode the app's sockaddr into the port's words. False when it is not an
 * AF_INET sockaddr. */
bool read_sockaddr(void const *address, int address_length, uint32_t *addr,
                   uint32_t *port) noexcept
{
    if (address == nullptr || address_length < static_cast<int>(kSockaddrInBytes)) {
        return false;
    }
    auto const *bytes = static_cast<uint8_t const *>(address);
    uint16_t const family = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
    if (family != kAfInet) {
        return false;
    }
    *port = static_cast<uint32_t>(swap16(static_cast<uint16_t>(bytes[2] | (bytes[3] << 8))));
    *addr = static_cast<uint32_t>(bytes[4]) | (static_cast<uint32_t>(bytes[5]) << 8) |
            (static_cast<uint32_t>(bytes[6]) << 16) | (static_cast<uint32_t>(bytes[7]) << 24);
    return true;
}

/* Encode the port's words into the app's sockaddr, when it gave one. */
void write_sockaddr(void *address, int *address_length, uint32_t addr,
                    uint32_t port) noexcept
{
    if (address == nullptr || address_length == nullptr ||
        *address_length < static_cast<int>(kSockaddrInBytes)) {
        return;
    }
    auto *bytes = static_cast<uint8_t *>(address);
    uint16_t const family = kAfInet;
    bytes[0] = static_cast<uint8_t>(family & 0xff);
    bytes[1] = static_cast<uint8_t>(family >> 8);
    uint16_t const network_port = swap16(static_cast<uint16_t>(port));
    bytes[2] = static_cast<uint8_t>(network_port & 0xff);
    bytes[3] = static_cast<uint8_t>(network_port >> 8);
    bytes[4] = static_cast<uint8_t>(addr & 0xff);
    bytes[5] = static_cast<uint8_t>((addr >> 8) & 0xff);
    bytes[6] = static_cast<uint8_t>((addr >> 16) & 0xff);
    bytes[7] = static_cast<uint8_t>((addr >> 24) & 0xff);
    for (uint32_t i = 8; i < kSockaddrInBytes; ++i) {
        bytes[i] = 0;
    }
    *address_length = static_cast<int>(kSockaddrInBytes);
}

/* The stack's socket id for an fd, or 0 when the fd is not a socket. */
uint32_t id_of(int fd) noexcept
{
    Entry *const entry = entry_for(fd);
    return entry != nullptr ? entry->id : 0;
}

}  // namespace

void adopt() noexcept
{
    g_sockets = aegir::ipc::Consumer::find(aegir::net::kPortName, aegir::net::kPortNameLength);
}

bool owns(int fd) noexcept
{
    return entry_for(fd) != nullptr;
}

long socket(int domain, int type, int protocol) noexcept
{
    if (domain != kAfInet || !g_sockets.valid()) {
        return -EAFNOSUPPORT;
    }
    uint32_t stack_type = 0;
    uint32_t stack_protocol = 0;
    if (type == kSockStream) {
        stack_type = aegir::net::kSockStream;
        stack_protocol = aegir::net::kIpprotoTcp;
    } else if (type == kSockDgram) {
        stack_type = aegir::net::kSockDgram;
        stack_protocol = aegir::net::kIpprotoUdp;
    } else if (type == kSockRaw) {
        stack_type = aegir::net::kSockRaw;
        stack_protocol = protocol != 0 ? static_cast<uint32_t>(protocol)
                                       : aegir::net::kIpprotoIcmp;
    } else {
        return -ESOCKTNOSUPPORT;
    }
    uint64_t const request[3] = {aegir::net::kAfInet, stack_type, stack_protocol};
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply = g_sockets.call_words(
        aegir::net::kMethodSocket, request, 3, answer, 1);
    if (reply.error != 0 || reply.count < 1 || answer[0] == 0) {
        return -EPROTONOSUPPORT;
    }
    int const fd = alloc_fd();
    if (fd < 0) {
        (void)g_sockets.call(aegir::net::kMethodClose, answer[0]);
        return -EMFILE;
    }
    Entry *const entry = entry_for(fd);
    entry->id = static_cast<uint32_t>(answer[0]);
    entry->family = static_cast<uint32_t>(domain);
    entry->type = static_cast<uint32_t>(type);
    entry->has_peer = false;
    entry->peer_address = 0;
    entry->peer_port = 0;
    return fd;
}

long bind(int fd, void const *address, int address_length) noexcept
{
    Entry *const entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    if (entry->type == kSockDgram || entry->type == kSockRaw) {
        /* A datagram socket is bound to an ephemeral port when it is made; an
         * explicit bind would need the port to carry one, which it does not
         * yet. Port 0 (any) is the common no-op and is accepted. */
        uint32_t addr = 0;
        uint32_t port = 0;
        if (address != nullptr && !read_sockaddr(address, address_length, &addr, &port)) {
            return -EINVAL;
        }
        return port == 0 ? 0 : -EOPNOTSUPP;
    }
    uint32_t addr = 0;
    uint32_t port = 0;
    if (!read_sockaddr(address, address_length, &addr, &port)) {
        return -EINVAL;
    }
    uint64_t const request[3] = {entry->id, addr, port};
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply = g_sockets.call_words(
        aegir::net::kMethodBind, request, 3, answer, 1);
    return reply.error == 0 && answer[0] == 1 ? 0 : -EADDRINUSE;
}

long listen(int fd, int backlog) noexcept
{
    (void)backlog;
    uint32_t const id = id_of(fd);
    if (id == 0) {
        return -EBADF;
    }
    uint64_t const request[1] = {id};
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply = g_sockets.call_words(
        aegir::net::kMethodListen, request, 1, answer, 1);
    return reply.error == 0 && answer[0] == 1 ? 0 : -EOPNOTSUPP;
}

long accept(int fd, void *address, int *address_length) noexcept
{
    uint32_t const id = id_of(fd);
    if (id == 0) {
        return -EBADF;
    }
    /* Wait forever: a blocking accept is the POSIX shape, and the port's held
     * reply is what makes it one. */
    uint64_t const request[2] = {id, 0};
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply = g_sockets.call_words(
        aegir::net::kMethodAccept, request, 2, answer, 1);
    if (reply.error != 0 || reply.count < 1 || answer[0] == 0) {
        return -EIO;
    }
    int const accepted = alloc_fd();
    if (accepted < 0) {
        (void)g_sockets.call(aegir::net::kMethodClose, answer[0]);
        return -EMFILE;
    }
    Entry *const entry = entry_for(accepted);
    entry->id = static_cast<uint32_t>(answer[0]);
    entry->family = kAfInet;
    entry->type = kSockStream;
    entry->has_peer = false;
    entry->peer_address = 0;
    entry->peer_port = 0;
    /* The port answers an accepted connection's id, not its peer; a caller
     * that wants the peer would have the stack carry it, next. */
    write_sockaddr(address, address_length, 0, 0);
    return accepted;
}

long connect(int fd, void const *address, int address_length) noexcept
{
    Entry *const entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    uint32_t addr = 0;
    uint32_t port = 0;
    if (!read_sockaddr(address, address_length, &addr, &port)) {
        return -EINVAL;
    }
    if (entry->type != kSockStream) {
        /* A datagram connect names the default peer; there is nothing on the
         * wire until a send, so it is recorded, not called. */
        entry->has_peer = true;
        entry->peer_address = addr;
        entry->peer_port = port;
        return 0;
    }
    uint64_t const request[4] = {entry->id, addr, port, 0};
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply = g_sockets.call_words(
        aegir::net::kMethodConnect, request, 4, answer, 1);
    if (reply.error != 0 || reply.count < 1 || answer[0] != 1) {
        return -ECONNREFUSED;
    }
    entry->has_peer = true;
    entry->peer_address = addr;
    entry->peer_port = port;
    return 0;
}

long sendto(int fd, void const *buffer, size_t length, int flags, void const *address,
            int address_length) noexcept
{
    (void)flags;
    Entry *const entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    if (length > aegir::net::kMaxPayloadWords * 8) {
        return -EMSGSIZE;
    }
    uint64_t request[4 + aegir::net::kMaxPayloadWords] = {0};
    uint32_t method = aegir::net::kMethodWrite;
    uint32_t words = 2;
    uint32_t payload_word = 2;
    request[0] = entry->id;
    request[1] = length;
    if (entry->type == kSockStream) {
        /* Stream: {id, length, payload...} */
        words = 2 + (length + 7) / 8;
    } else {
        uint32_t addr = entry->peer_address;
        uint32_t port = entry->peer_port;
        if (address != nullptr && !read_sockaddr(address, address_length, &addr, &port)) {
            return -EINVAL;
        }
        if (entry->type == kSockRaw) {
            /* Raw: {id, address, length, payload...} */
            method = aegir::net::kMethodSend;
            request[1] = addr;
            request[2] = length;
            words = 3 + (length + 7) / 8;
            payload_word = 3;
        } else {
            /* Datagram: {id, address, port, length, payload...} */
            method = aegir::net::kMethodSendTo;
            request[1] = addr;
            request[2] = port;
            request[3] = length;
            words = 4 + (length + 7) / 8;
            payload_word = 4;
        }
    }
    auto const *bytes = static_cast<uint8_t const *>(buffer);
    for (size_t i = 0; i < length; ++i) {
        request[payload_word + i / 8] |=
            static_cast<uint64_t>(bytes[i]) << (8 * (i % 8));
    }
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply =
        g_sockets.call_words(method, request, words, answer, 1);
    if (reply.error != 0 || reply.count < 1 || answer[0] == 0) {
        return -EIO;
    }
    return static_cast<long>(answer[0]);
}

long recvfrom(int fd, void *buffer, size_t length, int flags, void *address,
              int *address_length) noexcept
{
    (void)flags;
    Entry *const entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    uint64_t const request[2] = {entry->id, 0};
    uint64_t answer[kAnswerWords] = {0};
    uint32_t const method = entry->type == kSockDgram ? aegir::net::kMethodRecvFrom
                                                      : aegir::net::kMethodRecv;
    aegir::ipc::WordsReply const reply =
        g_sockets.call_words(method, request, 2, answer, kAnswerWords);
    if (reply.error != 0 || reply.count < 2) {
        return -EIO;
    }
    uint32_t header = entry->type == kSockDgram ? 3 : 2;
    if (reply.count < header) {
        return -EIO;
    }
    uint32_t const source = static_cast<uint32_t>(answer[0]);
    uint32_t const source_port = entry->type == kSockDgram
                                     ? static_cast<uint32_t>(answer[1])
                                     : 0;
    uint32_t const count = static_cast<uint32_t>(answer[header - 1]);
    if (count == 0) {
        /* Zero is end-of-stream, which a blocking POSIX recv reads as the peer
         * having closed. */
        return 0;
    }
    uint32_t taken = count;
    if (taken > length) {
        taken = static_cast<uint32_t>(length);
    }
    auto const *bytes = reinterpret_cast<uint8_t const *>(answer + header);
    auto *out = static_cast<uint8_t *>(buffer);
    for (uint32_t i = 0; i < taken; ++i) {
        out[i] = bytes[i];
    }
    write_sockaddr(address, address_length, source, source_port);
    return static_cast<long>(taken);
}

long shutdown(int fd, int how) noexcept
{
    (void)how;
    if (entry_for(fd) == nullptr) {
        return -EBADF;
    }
    /* A half-close has no port shape yet; accepting it without acting keeps a
     * caller that signals end-of-send from failing, and `close` sends the FIN. */
    return 0;
}

long getsockname(int fd, void *address, int *address_length) noexcept
{
    if (entry_for(fd) == nullptr) {
        return -EBADF;
    }
    write_sockaddr(address, address_length, 0, 0);
    return 0;
}

long getpeername(int fd, void *address, int *address_length) noexcept
{
    Entry *const entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    write_sockaddr(address, address_length, entry->peer_address, entry->peer_port);
    return 0;
}

long setsockopt(int fd, int level, int name, void const *value, int length) noexcept
{
    (void)level;
    (void)name;
    (void)value;
    (void)length;
    if (entry_for(fd) == nullptr) {
        return -EBADF;
    }
    /* Options the stack has no use for are accepted rather than refused: a
     * caller sets SO_REUSEADDR and friends before it knows better. */
    return 0;
}

long getsockopt(int fd, int level, int name, void *value, int *length) noexcept
{
    (void)name;
    if (entry_for(fd) == nullptr) {
        return -EBADF;
    }
    /* SO_ERROR is the one a caller checks; the rest the stack does not speak. */
    if (level == 1 /* SOL_SOCKET */ && value != nullptr && length != nullptr &&
        *length >= 4) {
        *static_cast<int *>(value) = 0;
        *length = 4;
        return 0;
    }
    return -ENOPROTOOPT;
}

long close(int fd) noexcept
{
    Entry *const entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    (void)g_sockets.call(aegir::net::kMethodClose, entry->id);
    entry->used = false;
    entry->id = 0;
    return 0;
}

uint32_t resolve(char const *name, size_t length, uint32_t timeout) noexcept
{
    if (!g_sockets.valid() || length == 0 || length > aegir::net::kMaxNameBytes) {
        return 0;
    }
    uint64_t request[2 + aegir::net::kMaxNameBytes / 8] = {0};
    request[0] = length;
    request[1] = timeout;
    for (size_t i = 0; i < length; ++i) {
        request[2 + i / 8] |= static_cast<uint64_t>(static_cast<uint8_t>(name[i]))
                              << (8 * (i % 8));
    }
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply = g_sockets.call_words(
        aegir::net::kMethodResolve, request, 2 + (length + 7) / 8, answer, 1);
    if (reply.error != 0 || reply.count < 1) {
        return 0;
    }
    return static_cast<uint32_t>(answer[0]);
}

}  // namespace aegir::network
