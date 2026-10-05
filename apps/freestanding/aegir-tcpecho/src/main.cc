/*
 * tcpecho: a TCP round trip over lwIP's loopback (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The socket port's stream slice, exercised end to end with no wire at all: a
 * listening socket on 127.0.0.1, a client socket that connects to it, the
 * connection accepted, a string written one way and echoed back, and the two
 * compared. It proves the whole lifecycle -- bind, listen, connect, accept,
 * write, recv -- and that an accepted connection is a socket of its own.
 *
 * It needs no concurrency: a TCP listener's backlog completes a connection
 * whether or not `accept` has been called, so the connect handshake finishes
 * and the accepted socket waits in the queue for the accept that follows. One
 * thread, one process, no host server, nothing on the wire.
 */

#include <aegir/bootstrap.h>
#include <aegir/console_stream.h>
#include <aegir/console_stream_client.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/net.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

/* The loopback address, 127.0.0.1 in network order (the low byte is 127). */
constexpr uint32_t kLoopback = 0x0100007Fu;
/* A port nothing else on the machine uses; the client and listener share it. */
constexpr uint32_t kPort = 4242;
constexpr uint32_t kTimeoutMs = 2000;
/* One segment's words: the string is far below this. */
constexpr uint32_t kPayloadWords = 16;

char const kMessage[] = "Aegir TCP echo";
constexpr uint32_t kMessageLength = sizeof(kMessage) - 1;

void write_line(char const *label, char const *text) noexcept
{
    aegir::debug_write("      ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

void pack(char const *text, uint32_t length, uint64_t *words) noexcept
{
    for (uint32_t i = 0; i < length; ++i) {
        words[i / 8] |= static_cast<uint64_t>(static_cast<uint8_t>(text[i]))
                        << (8 * (i % 8));
    }
}

void unpack(uint64_t const *words, uint32_t count, char *text, uint32_t length) noexcept
{
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t b = 0; b < 8; ++b) {
            uint32_t const at = i * 8 + b;
            if (at < length) {
                text[at] = static_cast<char>(words[i] >> (8 * b));
            }
        }
    }
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

uint32_t send_bytes(aegir::ipc::Consumer const &sockets, uint32_t id, char const *text,
                    uint32_t length) noexcept
{
    uint64_t request[2 + kPayloadWords] = {0};
    request[0] = id;
    request[1] = length;
    pack(text, length, request + 2);
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply = sockets.call_words(
        aegir::net::kMethodWrite, request, 2 + (length + 7) / 8, answer, 1);
    if (reply.error != 0 || reply.count < 1) {
        return 0;
    }
    return static_cast<uint32_t>(answer[0]);
}

/* Read one segment: its length, or 0 on timeout or end-of-stream. */
uint32_t recv_bytes(aegir::ipc::Consumer const &sockets, uint32_t id, char *out,
                    uint32_t capacity) noexcept
{
    uint64_t const request[2] = {id, kTimeoutMs};
    uint64_t answer[2 + kPayloadWords] = {0};
    aegir::ipc::WordsReply const reply = sockets.call_words(
        aegir::net::kMethodRecv, request, 2, answer, 2 + kPayloadWords);
    if (reply.error != 0 || reply.count < 2) {
        return 0;
    }
    uint32_t length = static_cast<uint32_t>(answer[1]);
    if (length > capacity) {
        length = capacity;
    }
    unpack(answer + 2, reply.count - 2, out, length);
    return length;
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
    if (!sockets.valid()) {
        write_line("FAIL", "tcpecho: no socket port was given to me");
        finish(1);
    }

    uint32_t const listener = make_socket(sockets);
    if (listener == 0) {
        write_line("FAIL", "tcpecho: the stack refused a listening socket");
        finish(1);
    }
    uint64_t const binding[3] = {listener, kLoopback, kPort};
    uint64_t bound[1] = {0};
    if (sockets.call_words(aegir::net::kMethodBind, binding, 3, bound, 1).error != 0 ||
        bound[0] != 1) {
        write_line("FAIL", "tcpecho: the listener would not bind");
        finish(1);
    }
    uint64_t const listening[1] = {listener};
    uint64_t listened[1] = {0};
    if (sockets.call_words(aegir::net::kMethodListen, listening, 1, listened, 1).error != 0 ||
        listened[0] != 1) {
        write_line("FAIL", "tcpecho: the listener would not listen");
        finish(1);
    }

    uint32_t const client = make_socket(sockets);
    if (client == 0) {
        write_line("FAIL", "tcpecho: the stack refused a client socket");
        finish(1);
    }
    uint64_t const connecting[4] = {client, kLoopback, kPort, kTimeoutMs};
    uint64_t connected[1] = {0};
    if (sockets.call_words(aegir::net::kMethodConnect, connecting, 4, connected, 1).error != 0 ||
        connected[0] != 1) {
        write_line("FAIL", "tcpecho: the connect did not complete");
        finish(1);
    }

    /* The accepted side: the connection is in the listener's backlog, so the
     * accept answers rather than waiting. */
    uint64_t const accepting[2] = {listener, kTimeoutMs};
    uint64_t accepted[1] = {0};
    if (sockets.call_words(aegir::net::kMethodAccept, accepting, 2, accepted, 1).error != 0 ||
        accepted[0] == 0) {
        write_line("FAIL", "tcpecho: no connection was accepted");
        finish(1);
    }
    uint32_t const server = static_cast<uint32_t>(accepted[0]);

    /* Out and back: the client writes, the accepted socket reads, echoes, and
     * the client reads what came back. */
    if (send_bytes(sockets, client, kMessage, kMessageLength) != kMessageLength) {
        write_line("FAIL", "tcpecho: the client's write was not taken");
        finish(1);
    }
    char received[kPayloadWords * 8];
    uint32_t const read_length = recv_bytes(sockets, server, received, sizeof(received));
    if (read_length != kMessageLength) {
        write_line("FAIL", "tcpecho: the accepted socket did not read the message");
        finish(1);
    }
    if (send_bytes(sockets, server, received, read_length) != read_length) {
        write_line("FAIL", "tcpecho: the echo was not taken");
        finish(1);
    }
    char echoed[kPayloadWords * 8];
    uint32_t const back_length = recv_bytes(sockets, client, echoed, sizeof(echoed));
    if (back_length != kMessageLength) {
        write_line("FAIL", "tcpecho: the client did not read the echo");
        finish(1);
    }
    for (uint32_t i = 0; i < kMessageLength; ++i) {
        if (echoed[i] != kMessage[i]) {
            write_line("FAIL", "tcpecho: the echo did not match");
            finish(1);
        }
    }

    (void)sockets.call(aegir::net::kMethodClose, client);
    (void)sockets.call(aegir::net::kMethodClose, server);
    (void)sockets.call(aegir::net::kMethodClose, listener);

    aegir::debug_write("      tcpecho: ");
    aegir::debug_write_unsigned(kMessageLength);
    aegir::debug_write(" bytes echoed over loopback: ");
    aegir::debug_write(echoed, kMessageLength);
    aegir::debug_write("\n");
    write_line("tcpecho", "ready");
    finish(0);
}
