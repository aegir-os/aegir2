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
    if (connect(client, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) < 0) {
        return fail("connect");
    }
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

    (void)close(server);
    (void)close(client);
    (void)close(listener);

    aegir::debug_write("  net-smoke: ");
    aegir::debug_write_unsigned(kMessageLength);
    aegir::debug_write(" bytes echoed through libc: ");
    aegir::debug_write(echoed, kMessageLength);
    aegir::debug_write("\n");
    aegir::debug_write("NET_SMOKE_OK\n");

    /* A plain return carries the status: the hosted runtime's exit reports it
     * and releases the command's memory (specs/shell.md's Phase 4). */
    return 0;
}
