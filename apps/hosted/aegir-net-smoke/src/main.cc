/*
 * aegir-net-smoke: the musl socket shim's acceptance client (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A hosted program that uses libc's socket calls -- `socket`, `bind`,
 * `listen`, `connect`, `accept`, `send`, `recv`, `close` -- and nothing of
 * Aegir's socket port directly. The shim (libs/hosted/aegir-network) turns
 * them into the stack's, so this proves the rerouting end to end: a listener
 * on 127.0.0.1, a client that connects, the connection accepted, a string out
 * and echoed back and compared. Loopback, so it needs no DHCP and touches no
 * wire.
 *
 * It stands the hosted runtime up like the aegir-print command (the untyped,
 * VSpace root and window the spawn kit gives a command), reports on the debug
 * serial, and returns from main -- the runtime's exit carries the status and
 * releases the command's memory, where a bare halt would leave the launcher
 * waiting.
 */

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <sel4/sel4.h>

namespace {

/* Static, like every hosted command's: the allocator's untyped table is tens
 * of kilobytes and a command's stack is pages. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

bool adopt_memory()
{
    uint64_t untyped_slot = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    uint64_t untyped_physical = 0;
    uint32_t untyped_bits = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(aegir::bootstrap::untyped(&untyped_physical, &untyped_bits,
                                                &untyped_address));

    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind == aegir::bootstrap::EntryKind::Capability &&
                entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        }
    }

    bool ok = aegir::bootstrap::capability("untyped", 7, &untyped_slot) &&
              aegir::bootstrap::capability("vspace", 6, &vspace_slot) &&
              aegir::bootstrap::window(&window_base, &window_bytes) &&
              g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                      untyped_physical);
    if (ok) {
        g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::kCNodeBits) - first_free, 0,
                              aegir::bootstrap::kCNodeBits);
        ok = g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                             static_cast<uintptr_t>(window_base),
                             static_cast<uintptr_t>(window_base + window_bytes), &g_objects);
    }
    return ok;
}

char const kMessage[] = "Aegir libc sockets";
uint32_t const kMessageLength = sizeof(kMessage) - 1;
/* A payload past the envelope (864 bytes), to exercise the bulk window. */
constexpr uint32_t kBulkBytes = 2000;

/* Report a failure and answer the status the command exits with. */
int fail(char const *what)
{
    aegir::debug_write("  net-smoke: FAIL ");
    aegir::debug_write(what);
    aegir::debug_write("\n");
    aegir::debug_write("NET_SMOKE_FAIL\n");
    return 1;
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("\nnet-smoke: sockets through libc\n");

    if (!adopt_memory()) {
        aegir::debug_write("  net-smoke: FAIL no untyped, vspace or window\n");
        aegir::debug_write("NET_SMOKE_FAIL\n");
        _Exit(127);
    }

    constexpr uint64_t kHeapBytes = 8ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("  net-smoke: FAIL the heap could not claim the window\n");
        aegir::debug_write("NET_SMOKE_FAIL\n");
        _Exit(127);
    }

    int const listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return fail("socket (listener)");
    }
    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(4242);
    local.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */
    if (bind(listener, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) < 0) {
        return fail("bind");
    }
    if (listen(listener, 1) < 0) {
        return fail("listen");
    }

    int const client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0) {
        return fail("socket (client)");
    }

    /* The peer's address comes from libc's own name lookup: `localhost` is a
     * name only Sys:S/hosts knows -- musl's getaddrinfo reads /etc/hosts and a
     * nameserver, neither of which this machine has -- so resolving it proves
     * the shim's override is the one linked. */
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *peer = nullptr;
    if (getaddrinfo("localhost", "4242", &hints, &peer) != 0 || peer == nullptr) {
        return fail("getaddrinfo");
    }
    auto const *peer_address = reinterpret_cast<struct sockaddr_in const *>(peer->ai_addr);
    uint32_t const resolved = peer_address->sin_addr.s_addr; /* low byte first */
    aegir::debug_write("  net-smoke: getaddrinfo localhost:4242 -> ");
    aegir::debug_write_unsigned(resolved & 0xff);
    aegir::debug_write(".");
    aegir::debug_write_unsigned((resolved >> 8) & 0xff);
    aegir::debug_write(".");
    aegir::debug_write_unsigned((resolved >> 16) & 0xff);
    aegir::debug_write(".");
    aegir::debug_write_unsigned((resolved >> 24) & 0xff);
    aegir::debug_write("\n");

    if (connect(client, peer->ai_addr, peer->ai_addrlen) < 0) {
        return fail("connect");
    }
    freeaddrinfo(peer);
    int const server = accept(listener, nullptr, nullptr);
    if (server < 0) {
        return fail("accept");
    }

    if (send(client, kMessage, kMessageLength, 0) != static_cast<long>(kMessageLength)) {
        return fail("send");
    }
    char received[64] = {};
    long const read_length = recv(server, received, sizeof(received), 0);
    if (read_length != static_cast<long>(kMessageLength)) {
        return fail("recv (accepted socket)");
    }
    if (send(server, received, static_cast<size_t>(read_length), 0) != read_length) {
        return fail("send (echo)");
    }
    char echoed[64] = {};
    long const back_length = recv(client, echoed, sizeof(echoed), 0);
    if (back_length != read_length) {
        return fail("recv (client)");
    }
    for (uint32_t i = 0; i < kMessageLength; ++i) {
        if (echoed[i] != kMessage[i]) {
            return fail("the echo did not match");
        }
    }

    /* The bulk path (specs/net.md): a payload past the envelope crosses in the
     * shim's own window, a frame it carves and hands the stack by capability.
     * Send it, read it back, compare. */
    uint8_t bulk[kBulkBytes] = {};
    for (uint32_t i = 0; i < kBulkBytes; ++i) {
        bulk[i] = static_cast<uint8_t>('a' + (i % 26));
    }
    long bulk_sent = 0;
    while (bulk_sent < static_cast<long>(kBulkBytes)) {
        long const n =
            send(client, bulk + bulk_sent, kBulkBytes - static_cast<uint32_t>(bulk_sent), 0);
        if (n <= 0) {
            return fail("bulk send");
        }
        bulk_sent += n;
    }
    uint8_t bulk_back[kBulkBytes] = {};
    long bulk_received = 0;
    while (bulk_received < static_cast<long>(kBulkBytes)) {
        long const n = recv(server, bulk_back + bulk_received,
                            kBulkBytes - static_cast<uint32_t>(bulk_received), 0);
        if (n <= 0) {
            return fail("bulk recv");
        }
        bulk_received += n;
    }
    for (uint32_t i = 0; i < kBulkBytes; ++i) {
        if (bulk_back[i] != bulk[i]) {
            return fail("the bulk payload did not match");
        }
    }
    aegir::debug_write("  net-smoke: ");
    aegir::debug_write_unsigned(kBulkBytes);
    aegir::debug_write(" bytes crossed the window through libc\n");

    (void)close(server);
    (void)close(client);
    (void)close(listener);

    aegir::debug_write("  net-smoke: ");
    aegir::debug_write_unsigned(kMessageLength);
    aegir::debug_write(" bytes echoed through libc: ");
    aegir::debug_write(echoed, kMessageLength);
    aegir::debug_write("\n");

    /* Leave one socket open on purpose. A client that dies does not close its
     * sockets, and they are the stack's objects, not its memory -- so the
     * launcher reaps the command's badge and the stack drops them
     * (aegir/net.h's kMethodReap). Exiting with this one open is the same path
     * a crash takes, without the crash. */
    int const abandoned = socket(AF_INET, SOCK_STREAM, 0);
    if (abandoned < 0) {
        return fail("socket (abandoned)");
    }
    aegir::debug_write("  net-smoke: leaving a socket open for the reap\n");
    aegir::debug_write("NET_SMOKE_OK\n");

    /* A plain return carries the status: the hosted runtime's exit reports it
     * and releases the command's memory (specs/shell.md's Phase 4). */
    return 0;
}
