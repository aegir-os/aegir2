/*
 * tftp: fetch a file from the DHCP-supplied gateway's TFTP server
 * (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The socket port's datagram slice, exercised end to end over the wire: the
 * client opens a UDP socket, sends a Read Request to the gateway QEMU's user-mode
 * network serves TFTP from, receives DATA blocks, and acknowledges each. It is
 * the "client socket path end to end" test the spec names -- still fully offline,
 * the server being the virtual host -- and it names no machine: the address is
 * the DHCP-supplied gateway, asked of the stack exactly as ping asks it.
 *
 * TFTP is small enough to read in the message envelope (a block is 512 bytes,
 * and the envelope carries more), so this lands the wire before the client's
 * shared window does; the window is TCP's, where payloads stop fitting.
 */

#include <aegir/bootstrap.h>
#include <aegir/console_stream.h>
#include <aegir/console_stream_client.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/net.h>
#include <aegir/netcontrol.h>
#include <aegir/timer.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

constexpr uint32_t kTftpPort = 69;
constexpr uint32_t kBlockBytes = 512;
/* How long a block is given before the transfer is declared dead. */
constexpr uint32_t kReplyTimeoutMs = 2000;
/* How long the gateway (and so the DHCP lease) is waited for: the same shape
 * ping waits with, because a Startup-Sequence client may run before the lease
 * has arrived. */
constexpr uint32_t kGatewayWaitAttempts = 25;
constexpr uint64_t kGatewayWaitNs = 250ull * 1000ull * 1000ull;
/* The first line captured for the acceptance, and its ceiling: a text line, not
 * a block. */
constexpr uint32_t kLineMax = 96;

void write_line(char const *label, char const *text) noexcept
{
    aegir::debug_write("      ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

void print_address(uint32_t address) noexcept
{
    for (uint32_t i = 0; i < 4; ++i) {
        if (i != 0) {
            aegir::debug_write(".");
        }
        aegir::debug_write_unsigned((address >> (8 * i)) & 0xff);
    }
}

uint32_t text_length(char const *text) noexcept
{
    uint32_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

uint16_t be16_get(uint8_t const *bytes) noexcept
{
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
}

void be16_put(uint8_t *bytes, uint16_t value) noexcept
{
    bytes[0] = static_cast<uint8_t>(value >> 8);
    bytes[1] = static_cast<uint8_t>(value & 0xff);
}

void unpack_words(uint64_t const *words, uint32_t count, uint8_t *bytes,
                  uint32_t length) noexcept
{
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t b = 0; b < 8; ++b) {
            uint32_t const at = i * 8 + b;
            if (at < length) {
                bytes[at] = static_cast<uint8_t>(words[i] >> (8 * b));
            }
        }
    }
}

/* The adapter's DHCP-supplied gateway, waited for the way ping waits: a
 * Startup-Sequence client may run before the lease arrived. */
bool find_gateway(aegir::ipc::Consumer const &control, aegir::ipc::Consumer const &timer,
                  uint32_t *gateway) noexcept
{
    for (unsigned attempt = 0; attempt < kGatewayWaitAttempts; ++attempt) {
        aegir::ipc::Reply const count = control.call(aegir::netcontrol::kMethodList, 0);
        for (uint64_t index = 0; count.error == 0 && index < count.word; ++index) {
            uint64_t answer[aegir::netcontrol::kDescribeWords];
            aegir::ipc::WordsReply const described = control.call_words(
                aegir::netcontrol::kMethodDescribe, &index, 1, answer,
                aegir::netcontrol::kDescribeWords);
            if (described.error == 0 &&
                described.count == aegir::netcontrol::kDescribeWords) {
                *gateway = static_cast<uint32_t>(answer[3]);
                if (*gateway != 0) {
                    return true;
                }
            }
        }
        if (timer.valid()) {
            (void)timer.call(aegir::timer::kMethodSleep, kGatewayWaitNs);
        }
    }
    return false;
}

}  // namespace

int main(int argc, char *argv[])
{
    char const *const name = argc > 1 && argv[1] != nullptr ? argv[1] : "aegir.txt";
    uint32_t const name_length = text_length(name);

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    aegir::ipc::Consumer const control = aegir::ipc::Consumer::find(
        aegir::netcontrol::kPortName, aegir::netcontrol::kPortNameLength);
    aegir::ipc::Consumer const sockets =
        aegir::ipc::Consumer::find(aegir::net::kPortName, aegir::net::kPortNameLength);
    aegir::ipc::Consumer const timer =
        aegir::ipc::Consumer::find(aegir::timer::kPortName, aegir::timer::kPortNameLength);
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

    if (!control.valid() || !sockets.valid()) {
        write_line("FAIL", "tftp: no control or socket port was given to me");
        finish(1);
    }
    if (name_length == 0 || name_length > 64) {
        write_line("FAIL", "tftp: the file name is 1 to 64 bytes");
        finish(1);
    }

    uint32_t gateway = 0;
    if (!find_gateway(control, timer, &gateway)) {
        write_line("FAIL", "tftp: the adapter has no gateway yet");
        finish(1);
    }

    uint64_t const open[3] = {aegir::net::kAfInet, aegir::net::kSockDgram,
                              aegir::net::kIpprotoUdp};
    uint64_t opened[1] = {0};
    aegir::ipc::WordsReply const made =
        sockets.call_words(aegir::net::kMethodSocket, open, 3, opened, 1);
    if (made.error != 0 || made.count < 1 || opened[0] == 0) {
        write_line("FAIL", "tftp: the stack refused a UDP socket");
        finish(1);
    }
    uint32_t const socket_id = static_cast<uint32_t>(opened[0]);

    /* The Read Request: opcode 1, the name, NUL, the mode "octet", NUL. */
    uint8_t request[2 + 64 + 1 + 5 + 1];
    uint32_t request_length = 0;
    be16_put(request + request_length, 1);
    request_length += 2;
    for (uint32_t i = 0; i < name_length; ++i) {
        request[request_length++] = static_cast<uint8_t>(name[i]);
    }
    request[request_length++] = 0;
    static char const kMode[] = "octet";
    for (uint32_t i = 0; i < sizeof(kMode) - 1; ++i) {
        request[request_length++] = static_cast<uint8_t>(kMode[i]);
    }
    request[request_length++] = 0;

    /* sendto's words: the id, the address, the port, the length, the payload. */
    uint64_t outgoing[4 + (sizeof(request) + 7) / 8] = {0};
    outgoing[0] = socket_id;
    outgoing[1] = gateway;
    outgoing[2] = kTftpPort;
    outgoing[3] = request_length;
    for (uint32_t i = 0; i < request_length; ++i) {
        outgoing[4 + i / 8] |=
            static_cast<uint64_t>(request[i]) << (8 * (i % 8));
    }
    uint64_t sent[1] = {0};
    aegir::ipc::WordsReply const transmitted = sockets.call_words(
        aegir::net::kMethodSendTo, outgoing, 4 + (request_length + 7) / 8, sent, 1);
    if (transmitted.error != 0 || sent[0] != request_length) {
        write_line("FAIL", "tftp: the read request would not go out");
        finish(1);
    }

    uint32_t server_address = gateway;
    uint32_t server_port = kTftpPort;
    uint32_t total = 0;
    uint32_t blocks = 0;
    uint32_t line_length = 0;
    uint32_t first_line_length = 0;
    char first_line[kLineMax];
    bool first_line_done = false;
    for (;;) {
        uint64_t request_words[2] = {socket_id, kReplyTimeoutMs};
        uint64_t answer[3 + aegir::net::kMaxPayloadWords] = {0};
        aegir::ipc::WordsReply const received = sockets.call_words(
            aegir::net::kMethodRecvFrom, request_words, 2, answer,
            3 + aegir::net::kMaxPayloadWords);
        if (received.error != 0 || received.count < 3) {
            write_line("FAIL", "tftp: no data block arrived");
            (void)sockets.call(aegir::net::kMethodClose, socket_id);
            finish(1);
        }
        server_address = static_cast<uint32_t>(answer[0]);
        server_port = static_cast<uint32_t>(answer[1]);
        uint32_t const length = static_cast<uint32_t>(answer[2]);
        if (length < 4 || length > kBlockBytes + 4) {
            write_line("FAIL", "tftp: a block was the wrong size");
            (void)sockets.call(aegir::net::kMethodClose, socket_id);
            finish(1);
        }
        uint8_t block[kBlockBytes + 4];
        unpack_words(answer + 3, received.count - 3, block, length);
        uint16_t const opcode = be16_get(block);
        if (opcode == 5) {
            write_line("FAIL", "tftp: the server refused the name");
            (void)sockets.call(aegir::net::kMethodClose, socket_id);
            finish(1);
        }
        if (opcode != 3) {
            write_line("FAIL", "tftp: the server sent something else");
            (void)sockets.call(aegir::net::kMethodClose, socket_id);
            finish(1);
        }
        uint32_t const data_length = length - 4;
        /* Keep the first line for the acceptance: the bytes the wire carried,
         * not merely how many. */
        if (!first_line_done) {
            for (uint32_t i = 0; i < data_length && !first_line_done; ++i) {
                char const c = static_cast<char>(block[4 + i]);
                if (c == '\n') {
                    first_line_done = true;
                } else if (line_length < kLineMax) {
                    first_line[line_length++] = c;
                } else {
                    first_line_done = true;
                }
            }
            first_line_length = line_length;
        }
        total += data_length;
        ++blocks;

        /* The acknowledgement, to the block's own sender: a TFTP server answers
         * a request from a fresh port and expects the ACK there. */
        uint8_t ack[4];
        be16_put(ack, 4);
        ack[2] = block[2];
        ack[3] = block[3];
        uint64_t acking[4 + 1] = {socket_id, server_address, server_port, 4};
        acking[4] = static_cast<uint64_t>(ack[0]) | (static_cast<uint64_t>(ack[1]) << 8) |
                    (static_cast<uint64_t>(ack[2]) << 16) |
                    (static_cast<uint64_t>(ack[3]) << 24);
        uint64_t acked[1] = {0};
        (void)sockets.call_words(aegir::net::kMethodSendTo, acking, 5, acked, 1);

        if (data_length < kBlockBytes) {
            break; /* a short block is the last */
        }
    }

    (void)sockets.call(aegir::net::kMethodClose, socket_id);

    aegir::debug_write("      tftp: ");
    aegir::debug_write_unsigned(total);
    aegir::debug_write(" bytes in ");
    aegir::debug_write_unsigned(blocks);
    aegir::debug_write(blocks == 1 ? " block from " : " blocks from ");
    print_address(server_address);
    aegir::debug_write("\n");
    aegir::debug_write("      tftp: first line: ");
    aegir::debug_write(first_line, first_line_length);
    aegir::debug_write("\n");

    write_line("tftp", "ready");
    finish(0);
}
