/*
 * aegir-ping: the socket port's first client (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * It is the plan's small landing: before TCP carries a connection state
 * machine, ping exercises the whole path -- create a raw ICMP socket, put an
 * echo request on the wire, receive the reply -- end to end. As a command it
 * takes an address or a name; with no argument -- as the boot acceptance runs
 * it -- it asks the stack's control port for the adapter's DHCP-supplied
 * gateway and pings that, which proves DHCP, ARP and ICMP together. Names
 * resolve through the stack's DNS today; Sys:S/hosts, the resolver's first
 * table, lands with the shared client library.
 */

#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/net.h>
#include <aegir/netcontrol.h>
#include <aegir/resolve.h>
#include <aegir/timer.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

void write_line(char const *label, char const *text) noexcept
{
    aegir::debug_write("      ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

/* The Internet checksum over a byte run, as the ICMP message needs it: the
 * ones-complement sum of 16-bit words, complemented. */
uint16_t internet_checksum(uint8_t const *bytes, uint32_t length) noexcept
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < length; i += 2) {
        sum += static_cast<uint32_t>(bytes[i]) | (static_cast<uint32_t>(bytes[i + 1]) << 8);
    }
    if ((length & 1) != 0) {
        sum += bytes[length - 1];
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

/* A datagram's bytes ride the message registers low byte first. */
void pack(uint8_t const *bytes, uint32_t length, uint64_t *words) noexcept
{
    for (uint32_t i = 0; i < (length + 7) / 8; ++i) {
        uint64_t word = 0;
        for (uint32_t b = 0; b < 8; ++b) {
            uint32_t const at = i * 8 + b;
            if (at < length) {
                word |= static_cast<uint64_t>(bytes[at]) << (8 * b);
            }
        }
        words[i] = word;
    }
}

void unpack(uint64_t const *words, uint32_t count, uint8_t *bytes,
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

void print_address(uint32_t address) noexcept
{
    for (uint32_t i = 0; i < 4; ++i) {
        if (i != 0) {
            aegir::debug_write(".");
        }
        aegir::debug_write_unsigned((address >> (8 * i)) & 0xff);
    }
}

constexpr uint32_t kEchoRequest = 8;
constexpr uint32_t kEchoReply = 0;
/* How long a `recv` waits before it gives up: a link that carries nothing (a
 * loopback that does not answer, a lost reply) must not hang the client. */
constexpr uint32_t kReplyTimeoutMs = 2000;
constexpr uint32_t kMessageBytes = 64;

}  // namespace

int main(int argc, char *argv[])
{
    /* A command takes a target on its command line; the boot acceptance runs it
     * with none and pings the adapter's gateway. It matters at the end: a command
     * must return from main so its launcher reaps it, where a boot service parks. */
    bool const has_argument = argc > 1 && argv[1] != nullptr && argv[1][0] != '\0';

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
    /* The console stream every command is handed: a launched command reports its
     * exit through it, and a command that only halts leaves the shell waiting so
     * the launcher never reaps it (aegir-echo's shape). A boot service has none,
     * and parks. */
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
        write_line("FAIL", "ping: no control or socket port was given to me");
        finish(1);
    }

    /* Where to ping: the command line's first argument (an address or a name),
     * or -- with none, as the boot acceptance runs it -- the adapter's
     * DHCP-supplied gateway, which may not have answered yet. */
    uint64_t target = 0;
    if (has_argument) {
        target = aegir::resolve::parse_ipv4(argv[1]);
        if (target == 0) {
            uint32_t length = 0;
            while (argv[1][length] != '\0') {
                ++length;
            }
            uint32_t resolved = 0;
            if (aegir::resolve::lookup(argv[1], length, &resolved)) {
                target = resolved;
            }
        }
        if (target == 0) {
            write_line("FAIL", "ping: the name would not resolve");
            finish(1);
        }
    } else {
        for (unsigned attempt = 0; attempt < 25 && target == 0; ++attempt) {
            aegir::ipc::Reply const count =
                control.call(aegir::netcontrol::kMethodList, 0);
            for (uint64_t index = 0; count.error == 0 && index < count.word; ++index) {
                uint64_t answer[aegir::netcontrol::kDescribeWords];
                aegir::ipc::WordsReply const described = control.call_words(
                    aegir::netcontrol::kMethodDescribe, &index, 1, answer,
                    aegir::netcontrol::kDescribeWords);
                if (described.error == 0 &&
                    described.count == aegir::netcontrol::kDescribeWords) {
                    target = answer[3]; /* the adapter's gateway */
                    if (target != 0) {
                        break;
                    }
                }
            }
            if (target == 0 && timer.valid()) {
                (void)timer.call(aegir::timer::kMethodSleep, 250ull * 1000ull * 1000ull);
            }
        }
        if (target == 0) {
            write_line("FAIL", "ping: the adapter has no gateway yet");
            finish(1);
        }
    }

    uint64_t const open[3] = {aegir::net::kAfInet, aegir::net::kSockRaw,
                              aegir::net::kIpprotoIcmp};
    uint64_t opened[1] = {0};
    aegir::ipc::WordsReply const made =
        sockets.call_words(aegir::net::kMethodSocket, open, 3, opened, 1);
    if (made.error != 0 || made.count < 1 || opened[0] == 0) {
        write_line("FAIL", "ping: the stack refused a raw ICMP socket");
        finish(1);
    }
    uint32_t const socket_id = static_cast<uint32_t>(opened[0]);

    /* An echo request: type, code, checksum, identifier, sequence, then data.
     * The checksum is the message's own (the raw socket adds no offload). */
    uint8_t message[kMessageBytes];
    for (uint32_t i = 0; i < kMessageBytes; ++i) {
        message[i] = 0;
    }
    message[0] = kEchoRequest;
    message[1] = 0;
    message[4] = 0xae;
    message[5] = 0x91; /* our identifier */
    message[6] = 0;
    message[7] = 1; /* sequence */
    char const greeting[] = "aegir";
    for (uint32_t i = 8; i < kMessageBytes; ++i) {
        message[i] = static_cast<uint8_t>(greeting[(i - 8) % (sizeof(greeting) - 1)]);
    }
    uint16_t const sum = internet_checksum(message, kMessageBytes);
    message[2] = static_cast<uint8_t>(sum & 0xff);
    message[3] = static_cast<uint8_t>((sum >> 8) & 0xff);

    uint64_t request[3 + (kMessageBytes + 7) / 8] = {0};
    request[0] = socket_id;
    request[1] = target;
    request[2] = kMessageBytes;
    pack(message, kMessageBytes, request + 3);
    uint64_t sent[1] = {0};
    aegir::ipc::WordsReply const transmitted = sockets.call_words(
        aegir::net::kMethodSend, request, 3 + (kMessageBytes + 7) / 8, sent, 1);
    if (transmitted.error != 0 || sent[0] != kMessageBytes) {
        write_line("FAIL", "ping: the echo request would not go out");
        (void)sockets.call(aegir::net::kMethodClose, socket_id);
        finish(1);
    }

    /* The reply is held until it arrives. A raw ICMP socket sees every ICMP
     * message, so one that is not an echo reply -- the request itself, looped
     * back on 127.0.0.1 -- is skipped rather than printed. */
    uint64_t identifier[2] = {socket_id, kReplyTimeoutMs};
    uint8_t answered[(kMessageBytes + 7) / 8 * 8];
    uint32_t source = 0;
    uint32_t length = 0;
    bool got_reply = false;
    for (unsigned attempt = 0; attempt < 8 && !got_reply; ++attempt) {
        uint64_t reply[2 + (kMessageBytes + 7) / 8] = {0};
        aegir::ipc::WordsReply const received = sockets.call_words(
            aegir::net::kMethodRecv, identifier, 2, reply, 2 + (kMessageBytes + 7) / 8);
        if (received.error != 0) {
            break;
        }
        if (received.count < 2 || reply[1] == 0) {
            continue;
        }
        length = static_cast<uint32_t>(reply[1]);
        if (length > sizeof(answered)) {
            length = sizeof(answered);
        }
        unpack(reply + 2, received.count - 2, answered, length);
        if (length >= 1 && answered[0] == kEchoReply) {
            source = static_cast<uint32_t>(reply[0]);
            got_reply = true;
        }
    }
    if (!got_reply) {
        write_line("FAIL", "ping: no echo reply arrived");
        (void)sockets.call(aegir::net::kMethodClose, socket_id);
        finish(1);
    }

    aegir::debug_write("      ping: reply from ");
    print_address(source);
    aegir::debug_write(", type ");
    aegir::debug_write_unsigned(answered[0]);
    aegir::debug_write(", ");
    aegir::debug_write_unsigned(length);
    aegir::debug_write(" bytes\n");
    (void)sockets.call(aegir::net::kMethodClose, socket_id);

    write_line("ping", "ready");
    finish(0);
}
