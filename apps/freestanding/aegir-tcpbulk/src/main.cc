/*
 * tcpbulk: a bulk TCP round trip over lwIP's loopback (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The socket port's bulk path: a payload larger than the message registers
 * crosses in a window the client owns. The client carves one frame, mints a
 * pristine copy for the stack to map (before mapping its own), writes the
 * payload into the window, sends it with `write-window`, reads it back with
 * `recv-window`, and compares -- the whole way through a window and never a
 * register. Loopback, so nothing touches the wire.
 */

#include <aegir/bootstrap.h>
#include <aegir/console_stream.h>
#include <aegir/console_stream_client.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/net.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

constexpr uint32_t kLoopback = 0x0100007Fu; /* 127.0.0.1, low byte first */
constexpr uint32_t kPort = 4243;
constexpr uint32_t kTimeoutMs = 2000;
/* A payload past the envelope (864 bytes) but inside one window page. */
constexpr uint32_t kBulkBytes = 3000;
constexpr uint32_t kWindowBytes = 4096;

aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

uint64_t first_free_slot() noexcept
{
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
    return first_free;
}

bool adopt_memory() noexcept
{
    uint64_t untyped_slot = 0;
    uint32_t untyped_bits = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    if (!aegir::bootstrap::capability("untyped", 7, &untyped_slot) ||
        !aegir::bootstrap::capability_size_bits("untyped", 7, &untyped_bits) ||
        !aegir::bootstrap::capability("vspace", 6, &vspace_slot) ||
        !aegir::bootstrap::window(&window_base, &window_bytes)) {
        return false;
    }
    if (!g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits)) {
        return false;
    }
    uint64_t const first_free = first_free_slot();
    g_objects.adopt_slots(static_cast<seL4_CPtr>(first_free),
                          (1u << aegir::bootstrap::kCNodeBits) -
                              static_cast<uint32_t>(first_free),
                          0, aegir::bootstrap::kCNodeBits);
    return g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                           static_cast<uintptr_t>(window_base),
                           static_cast<uintptr_t>(window_base + window_bytes), &g_objects);
}

void write_line(char const *label, char const *text) noexcept
{
    aegir::debug_write("      ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

uint32_t make_socket(aegir::ipc::Consumer const &sockets) noexcept
{
    uint64_t const open[3] = {aegir::net::kAfInet, aegir::net::kSockStream,
                              aegir::net::kIpprotoTcp};
    uint64_t opened[1] = {0};
    aegir::ipc::WordsReply const made =
        sockets.call_words(aegir::net::kMethodSocket, open, 3, opened, 1);
    if (made.error != 0 || made.count < 1) {
        return 0;
    }
    return static_cast<uint32_t>(opened[0]);
}

bool bind_socket(aegir::ipc::Consumer const &sockets, uint32_t id) noexcept
{
    uint64_t const request[3] = {id, kLoopback, kPort};
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply =
        sockets.call_words(aegir::net::kMethodBind, request, 3, answer, 1);
    return reply.error == 0 && answer[0] == 1;
}

bool listen_socket(aegir::ipc::Consumer const &sockets, uint32_t id) noexcept
{
    uint64_t const request[1] = {id};
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply =
        sockets.call_words(aegir::net::kMethodListen, request, 1, answer, 1);
    return reply.error == 0 && answer[0] == 1;
}

uint32_t connect_socket(aegir::ipc::Consumer const &sockets, uint32_t id) noexcept
{
    uint64_t const request[4] = {id, kLoopback, kPort, kTimeoutMs};
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply =
        sockets.call_words(aegir::net::kMethodConnect, request, 4, answer, 1);
    if (reply.error != 0 || reply.count < 1 || answer[0] != 1) {
        return 0;
    }
    return 1;
}

uint32_t accept_socket(aegir::ipc::Consumer const &sockets, uint32_t id) noexcept
{
    uint64_t const request[2] = {id, kTimeoutMs};
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply =
        sockets.call_words(aegir::net::kMethodAccept, request, 2, answer, 1);
    if (reply.error != 0 || reply.count < 1) {
        return 0;
    }
    return static_cast<uint32_t>(answer[0]);
}

/* Send the window's `length` bytes by transferring the window cap with the
 * call. */
uint32_t write_window(aegir::ipc::Consumer const &sockets, uint32_t id,
                      seL4_CPtr window_cap, uint32_t length) noexcept
{
    uint64_t const request[2] = {id, length};
    uint64_t answer[1] = {0};
    bool cap_received = false;
    aegir::ipc::WordsReply const reply =
        sockets.call_transfer(aegir::net::kMethodWriteWindow, request, 2, window_cap,
                              answer, 1, &cap_received);
    if (reply.error != 0 || reply.count < 1) {
        return 0;
    }
    return static_cast<uint32_t>(answer[0]);
}

/* Read up to `capacity` bytes into the window: the answer is the length now
 * there, zero on timeout or end-of-stream. */
uint32_t recv_window(aegir::ipc::Consumer const &sockets, uint32_t id,
                     seL4_CPtr window_cap, uint32_t capacity) noexcept
{
    uint64_t const request[3] = {id, kTimeoutMs, capacity};
    uint64_t answer[1] = {0};
    bool cap_received = false;
    aegir::ipc::WordsReply const reply =
        sockets.call_transfer(aegir::net::kMethodRecvWindow, request, 3, window_cap,
                              answer, 1, &cap_received);
    if (reply.error != 0 || reply.count < 1) {
        return 0;
    }
    return static_cast<uint32_t>(answer[0]);
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    aegir::ipc::Consumer const sockets =
        aegir::ipc::Consumer::find(aegir::net::kPortName, aegir::net::kPortNameLength);
    aegir::ipc::Consumer const stream = aegir::ipc::Consumer::find(
        aegir::console::kStreamPortName, aegir::console::kStreamPortNameLength);
    auto finish = [&](uint64_t status) {
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        if (stream.valid()) {
            uint64_t badge = 0;
            (void)aegir::bootstrap::badge(&badge);
            (void)aegir::console::stream_exit(stream, status, badge);
        }
        aegir::halt();
    };
    if (!sockets.valid() || !adopt_memory()) {
        write_line("FAIL", "tcpbulk: no socket port or no memory");
        finish(1);
    }

    /* The window: one frame, a pristine copy the stack maps (minted before we
     * map our own, or the copy would be pinned to our ASID), and our own
     * mapping to write and read through. */
    aegir::mem::Account account{"window", 0, 0, 0};
    seL4_Error error = seL4_NoError;
    seL4_CPtr const frame = g_objects.alloc_page(account, &error);
    seL4_CPtr const window_cap = g_objects.alloc_slot();
    if (frame == 0 || window_cap == 0 ||
        seL4_CNode_Copy(aegir::bootstrap::kSlotOwnCNode, window_cap,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        frame, aegir::bootstrap::kCNodeBits,
                        seL4_AllRights) != seL4_NoError) {
        write_line("FAIL", "tcpbulk: the window would not be carved");
        finish(1);
    }
    auto *const window = static_cast<uint8_t *>(g_scratch.map(frame));
    if (window == nullptr) {
        write_line("FAIL", "tcpbulk: the window would not be mapped");
        finish(1);
    }
    /* The payload, and the same bytes kept to compare against. */
    uint8_t expected[kBulkBytes] = {};
    for (uint32_t i = 0; i < kBulkBytes; ++i) {
        expected[i] = static_cast<uint8_t>('A' + (i % 26));
        window[i] = expected[i];
    }

    uint32_t const listener = make_socket(sockets);
    if (listener == 0 || !bind_socket(sockets, listener) || !listen_socket(sockets, listener)) {
        write_line("FAIL", "tcpbulk: the listener would not bind and listen");
        finish(1);
    }
    uint32_t const client = make_socket(sockets);
    if (client == 0 || connect_socket(sockets, client) == 0) {
        write_line("FAIL", "tcpbulk: the client would not connect");
        finish(1);
    }
    uint32_t const server = accept_socket(sockets, listener);
    if (server == 0) {
        write_line("FAIL", "tcpbulk: the connection was not accepted");
        finish(1);
    }

    uint32_t const sent = write_window(sockets, client, window_cap, kBulkBytes);
    if (sent != kBulkBytes) {
        aegir::debug_write("      tcpbulk: the bulk write took ");
        aegir::debug_write_unsigned(sent);
        aegir::debug_write("\n");
        write_line("FAIL", "tcpbulk: the bulk write was not taken whole");
        finish(1);
    }

    /* Read it back into the same window, a call at a time, comparing as it
     * arrives. */
    uint32_t received = 0;
    while (received < kBulkBytes) {
        uint32_t const got = recv_window(sockets, server, window_cap, kWindowBytes);
        if (got == 0) {
            break;
        }
        for (uint32_t i = 0; i < got && received + i < kBulkBytes; ++i) {
            if (window[i] != expected[received + i]) {
                write_line("FAIL", "tcpbulk: the bulk echo did not match");
                finish(1);
            }
        }
        received += got;
    }
    if (received != kBulkBytes) {
        write_line("FAIL", "tcpbulk: the bulk echo was short");
        finish(1);
    }

    aegir::debug_write("      tcpbulk: ");
    aegir::debug_write_unsigned(received);
    aegir::debug_write(" bytes crossed the window\n");
    aegir::debug_write("      TCPBULK_OK\n");
    finish(0);
}
