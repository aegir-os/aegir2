/*
 * aegir-net's socket half: see socket.h and specs/net.md.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "socket.h"

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/net.h>

#include "interface.h"

#include <sel4/sel4.h>
#include <stdint.h>

extern "C" {
#include <lwip/dns.h>
#include <lwip/ip.h>
#include <lwip/ip_addr.h>
#include <lwip/mem.h>
#include <lwip/pbuf.h>
#include <lwip/prot/ip.h>
#include <lwip/raw.h>
#include <lwip/sys.h>
#include <lwip/tcp.h>
#include <lwip/tcpbase.h>
#include <lwip/tcpip.h>
#include <lwip/timeouts.h>
#include <lwip/udp.h>
}

namespace aegir::net {
namespace {

/* What a socket is: a raw ICMP PCB, a UDP one, or a TCP connection. The three
 * answer `recv`/`recvfrom` in their own layouts -- a datagram carries its peer's
 * port, an ICMP message does not, a stream carries neither -- so the kind
 * travels with the waiting reply. */
constexpr uint32_t kRaw = 0;
constexpr uint32_t kUdp = 1;
constexpr uint32_t kStream = 2;

/* What a held reply is waiting for. A stream socket holds for more than one
 * thing -- an accepted connection, a completed connect, a segment -- and each
 * answers in its own shape, so the kind is recorded with the slot. */
constexpr uint32_t kHoldNone = 0;
constexpr uint32_t kHoldRecv = 1;    /* recv/recvfrom: source, [port,] length, bytes */
constexpr uint32_t kHoldConnect = 2; /* connect: 1 up, 0 refused */
constexpr uint32_t kHoldAccept = 3;  /* accept: the new socket's id */

/* The bytes the message envelope carries after the protocol's own words. */
constexpr uint32_t kEnvelopeBytes = kMaxPayloadWords * 8;

/* A received datagram waiting for its socket's next `recv`: the source address,
 * the source port (UDP; zero otherwise), the length, and the bytes. */
struct Datagram {
    uint32_t source;
    uint32_t source_port;
    uint32_t length;
    Datagram *next;
    uint8_t payload[];
};

/* One connection a listener has accepted but `accept` has not taken yet. */
struct Accepted {
    uint32_t id;
    Accepted *next;
};

/* One open socket: the lwIP PCB (one of the three, by kind), the reply slot a
 * waited call saved (0 when the client is not waiting) and what that call was,
 * datagrams that arrived with no waiter, and -- for a listener -- connections
 * accepted but not yet taken. The held slot and the queues cross between this
 * thread and the tcpip thread, so they are touched atomically. */
struct Socket {
    uint32_t id;
    uint32_t kind;
    uint64_t owner; /* the badge that made it -- the reap key (aegir/net.h) */
    struct raw_pcb *raw;
    struct udp_pcb *udp;
    struct tcp_pcb *tcp;
    volatile uintptr_t held;
    volatile uint32_t held_kind;
    /* Datagrams that arrived with no waiter, oldest first. A raw ICMP socket
     * sees more than one message per exchange -- the looped request and its
     * reply -- so one slot would drop all but the first. */
    Datagram *pending_head;
    Datagram *pending_tail;
    Accepted *accept_head;
    Accepted *accept_tail;
    Socket *next;
};

Socket *g_sockets = nullptr;
uint32_t g_next_id = 1;
aegir::mem::Allocator *g_objects = nullptr;
aegir::mem::Account *g_account = nullptr;

/* The socket list and its id counter cross this thread and the tcpip thread -- a
 * listener's accept callback adds a connection -- so a short spinlock guards
 * them. */
volatile int g_list_lock = 0;

void lock_list() noexcept
{
    while (__atomic_test_and_set(&g_list_lock, __ATOMIC_ACQUIRE)) {
    }
}
void unlock_list() noexcept
{
    __atomic_clear(&g_list_lock, __ATOMIC_RELEASE);
}

void list_add(Socket *socket) noexcept
{
    lock_list();
    socket->id = g_next_id++;
    socket->next = g_sockets;
    g_sockets = socket;
    unlock_list();
}

/* The pending queues cross this thread (a `recv`) and the tcpip thread (a
 * reply), so a short spinlock guards them. */
volatile int g_queue_lock = 0;

void lock_queue() noexcept
{
    while (__atomic_test_and_set(&g_queue_lock, __ATOMIC_ACQUIRE)) {
    }
}
void unlock_queue() noexcept
{
    __atomic_clear(&g_queue_lock, __ATOMIC_RELEASE);
}

void queue_push(Socket *socket, Datagram *datagram) noexcept
{
    lock_queue();
    datagram->next = nullptr;
    if (socket->pending_tail != nullptr) {
        socket->pending_tail->next = datagram;
    } else {
        socket->pending_head = datagram;
    }
    socket->pending_tail = datagram;
    unlock_queue();
}

Datagram *queue_pop(Socket *socket) noexcept
{
    lock_queue();
    Datagram *const datagram = socket->pending_head;
    if (datagram != nullptr) {
        socket->pending_head = datagram->next;
        if (socket->pending_head == nullptr) {
            socket->pending_tail = nullptr;
        }
    }
    unlock_queue();
    return datagram;
}

/* The accept queue: connections a listener has accepted but `accept` has not
 * taken. The tcpip thread pushes, this thread pops, so the queue lock guards
 * it. */
void accept_push(Socket *listener, uint32_t id) noexcept
{
    auto *const node = static_cast<Accepted *>(mem_malloc(sizeof(Accepted)));
    if (node == nullptr) {
        return;
    }
    node->id = id;
    node->next = nullptr;
    lock_queue();
    if (listener->accept_tail != nullptr) {
        listener->accept_tail->next = node;
    } else {
        listener->accept_head = node;
    }
    listener->accept_tail = node;
    unlock_queue();
}

uint32_t accept_pop(Socket *listener) noexcept
{
    lock_queue();
    Accepted *const node = listener->accept_head;
    uint32_t id = 0;
    if (node != nullptr) {
        listener->accept_head = node->next;
        if (listener->accept_head == nullptr) {
            listener->accept_tail = nullptr;
        }
        id = node->id;
    }
    unlock_queue();
    if (node != nullptr) {
        mem_free(node);
    }
    return id;
}

/* Reply slots, reused: a `recv`/`resolve` takes one, the answer puts it back.
 * Both this thread and the tcpip thread touch the pool, so a short spinlock
 * guards it. New slots come from the allocator when the pool is empty -- it
 * grows on demand, and never shrinks. */
seL4_CPtr *g_free_slots = nullptr;
uint32_t g_free_count = 0;
uint32_t g_free_capacity = 0;
volatile int g_slot_lock = 0;

void lock_slots() noexcept
{
    while (__atomic_test_and_set(&g_slot_lock, __ATOMIC_ACQUIRE)) {
    }
}
void unlock_slots() noexcept
{
    __atomic_clear(&g_slot_lock, __ATOMIC_RELEASE);
}

seL4_CPtr slot_get() noexcept
{
    lock_slots();
    if (g_free_count != 0) {
        seL4_CPtr const slot = g_free_slots[--g_free_count];
        unlock_slots();
        return slot;
    }
    unlock_slots();
    return g_objects->alloc_slot();
}

void slot_put(seL4_CPtr slot) noexcept
{
    lock_slots();
    if (g_free_count == g_free_capacity) {
        uint32_t const capacity = g_free_capacity != 0 ? g_free_capacity * 2 : 16;
        auto *const grown =
            static_cast<seL4_CPtr *>(mem_malloc(sizeof(seL4_CPtr) * capacity));
        if (grown == nullptr) {
            unlock_slots();
            return; /* the slot is dropped rather than the pool corrupted */
        }
        for (uint32_t i = 0; i < g_free_count; ++i) {
            grown[i] = g_free_slots[i];
        }
        if (g_free_slots != nullptr) {
            mem_free(g_free_slots);
        }
        g_free_slots = grown;
        g_free_capacity = capacity;
    }
    g_free_slots[g_free_count++] = slot;
    unlock_slots();
}

/* Words and bytes: a payload's bytes ride the message registers low byte first.
 * The length is carried separately, so trailing zeros in the last word are not
 * part of the datagram. */
uint32_t pack_bytes(uint8_t const *bytes, uint32_t length, uint64_t *words) noexcept
{
    uint32_t const count = (length + 7) / 8;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t word = 0;
        for (uint32_t b = 0; b < 8; ++b) {
            uint32_t const at = i * 8 + b;
            if (at < length) {
                word |= static_cast<uint64_t>(bytes[at]) << (8 * b);
            }
        }
        words[i] = word;
    }
    return count;
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

uint32_t ip_word(ip_addr_t const *address) noexcept
{
    return ip_2_ip4(address)->addr;
}

/* A call into the tcpip thread, run synchronously: post it and wait on a
 * semaphore the thunk signals (the same shape `link_configure` uses). */
struct Job {
    void (*run)(void *);
    void *argument;
    sys_sem_t done;
};

void job_thunk(void *argument) noexcept
{
    auto *const job = static_cast<Job *>(argument);
    job->run(job->argument);
    sys_sem_signal(&job->done);
}

bool in_tcpip(void (*run)(void *), void *argument) noexcept
{
    Job job{run, argument, nullptr};
    if (sys_sem_new(&job.done, 0) != ERR_OK) {
        return false;
    }
    if (tcpip_callback(job_thunk, &job) != ERR_OK) {
        sys_sem_free(&job.done);
        return false;
    }
    sys_arch_sem_wait(&job.done, 0);
    sys_sem_free(&job.done);
    return true;
}

/* Answer a held reply cap (in the shared CSpace) with one word, and give its
 * slot back. Run from the tcpip thread, where the answer originates. */
void answer_held(seL4_CPtr held, uint64_t word) noexcept
{
    seL4_SetMR(0, word);
    seL4_Send(held, seL4_MessageInfo_new(0, 0, 0, 1));
    slot_put(held);
}

/* Where the IP payload starts: lwIP hands a raw socket the whole IP packet, but
 * this port's `send` takes the protocol payload (the stack adds the IP header),
 * so `recv` is symmetric and gives the payload too. The IP header's length is
 * its first byte's low nibble, in 32-bit words. */
uint32_t ip_payload_offset(struct pbuf *packet) noexcept
{
    uint8_t first = 0;
    if (pbuf_copy_partial(packet, &first, 1, 0) != 1) {
        return 0;
    }
    uint32_t const header = static_cast<uint32_t>(first & 0x0f) * 4;
    return header <= packet->tot_len ? header : 0;
}

/* Hand a received message to a waiting `recv`/`recvfrom`, or queue it. Runs in
 * the tcpip thread. The answer's shape follows the socket's kind: a datagram
 * carries its peer's port, an ICMP message does not, a stream carries neither.
 * A zero length -- a peer's close -- carries no bytes but still wakes the
 * waiter. The bytes are copied straight into the message registers, not into an
 * array and then from the array into the registers. */
void deliver(Socket *socket, uint32_t source, uint32_t source_port,
             struct pbuf *packet, uint32_t offset, uint32_t length) noexcept
{
    if (__atomic_load_n(&socket->held_kind, __ATOMIC_ACQUIRE) == kHoldRecv) {
        uintptr_t const held = __atomic_exchange_n(&socket->held, 0, __ATOMIC_ACQ_REL);
        if (held != 0) {
            seL4_Word *const message = seL4_GetIPCBuffer()->msg;
            uint32_t at = 2;
            message[0] = source;
            if (socket->kind == kUdp) {
                message[1] = source_port;
                message[2] = length;
                at = 3;
            } else {
                message[1] = length;
            }
            if (length != 0) {
                pbuf_copy_partial(packet, reinterpret_cast<uint8_t *>(message + at),
                                  static_cast<u16_t>(length), static_cast<u16_t>(offset));
            }
            seL4_Send(held, seL4_MessageInfo_new(0, 0, 0, at + (length + 7) / 8));
            slot_put(static_cast<seL4_CPtr>(held));
            return;
        }
    }
    auto *const datagram =
        static_cast<Datagram *>(mem_malloc(sizeof(Datagram) + length));
    if (datagram != nullptr) {
        datagram->source = source;
        datagram->source_port = source_port;
        datagram->length = length;
        datagram->next = nullptr;
        if (length != 0) {
            pbuf_copy_partial(packet, datagram->payload, static_cast<u16_t>(length),
                              static_cast<u16_t>(offset));
        }
        queue_push(socket, datagram);
    }
}

/* Answer a held connect or accept with one word, when that is what is waiting.
 * Runs in the tcpip thread. */
void answer_flag(Socket *socket, uint32_t kind, uint64_t value) noexcept
{
    if (__atomic_load_n(&socket->held_kind, __ATOMIC_ACQUIRE) != kind) {
        return;
    }
    uintptr_t const held = __atomic_exchange_n(&socket->held, 0, __ATOMIC_ACQ_REL);
    if (held == 0) {
        return;
    }
    seL4_SetMR(0, value);
    seL4_Send(held, seL4_MessageInfo_new(0, 0, 0, 1));
    slot_put(static_cast<seL4_CPtr>(held));
}

/* A stream segment, split into envelope-sized pieces so a segment larger than
 * the registers is not truncated: each piece is a `recv` on its own. A
 * zero-length segment -- the peer's close -- is one piece. */
void deliver_stream(Socket *socket, struct pbuf *packet, uint32_t length) noexcept
{
    uint32_t offset = 0;
    uint32_t remaining = length;
    do {
        uint32_t const chunk = remaining > kEnvelopeBytes ? kEnvelopeBytes : remaining;
        deliver(socket, 0, 0, packet, offset, chunk);
        offset += chunk;
        remaining -= chunk;
    } while (remaining > 0);
}

/* A reply from the link: answer the waiting `recv`, or keep the datagram for
 * the next one. Runs in the tcpip thread. */
u8_t raw_received(void *argument, struct raw_pcb *pcb, struct pbuf *packet,
                  ip_addr_t const *source) noexcept
{
    static_cast<void>(pcb);
    auto *const socket = static_cast<Socket *>(argument);
    uint32_t const offset = ip_payload_offset(packet);
    uint32_t length = packet->tot_len - offset;
    if (length > kEnvelopeBytes) {
        length = kEnvelopeBytes;
    }
    deliver(socket, ip_word(source), 0, packet, offset, length);
    /* Not consumed: lwIP's ICMP layer must still see the packet, or an echo
     * request to a local address -- 127.0.0.1, looped back -- is never answered,
     * because raw_input runs first and eating the packet stops the chain. The
     * socket has its copy either way, so ping still sees the request and then
     * the reply. */
    return 0;
}

/* A UDP datagram: the pbuf is this socket's now, with no IP header before the
 * payload, and the callback owns it -- so it is freed here, after the copy.
 * Runs in the tcpip thread. */
void udp_received(void *argument, struct udp_pcb *pcb, struct pbuf *packet,
                  ip_addr_t const *source, u16_t source_port) noexcept
{
    static_cast<void>(pcb);
    auto *const socket = static_cast<Socket *>(argument);
    uint32_t length = packet->tot_len;
    if (length > kEnvelopeBytes) {
        length = kEnvelopeBytes;
    }
    deliver(socket, ip_word(source), source_port, packet, 0, length);
    pbuf_free(packet);
}

/* A `recv` no packet answered: answer the held reply with a zero length -- a
 * timeout, which a client reads as end-of-stream -- and give the slot back. Runs
 * in the tcpip thread from lwIP's timers. */
void recv_timeout(void *argument) noexcept
{
    auto *const socket = static_cast<Socket *>(argument);
    if (__atomic_load_n(&socket->held_kind, __ATOMIC_ACQUIRE) != kHoldRecv) {
        return;
    }
    uintptr_t const held = __atomic_exchange_n(&socket->held, 0, __ATOMIC_ACQ_REL);
    if (held == 0) {
        return;
    }
    seL4_SetMR(0, 0);
    uint32_t words = 1;
    if (socket->kind == kUdp) {
        seL4_SetMR(1, 0);
        seL4_SetMR(2, 0);
        words = 3;
    } else if (socket->kind == kStream) {
        seL4_SetMR(1, 0);
        words = 2;
    }
    seL4_Send(held, seL4_MessageInfo_new(0, 0, 0, words));
    slot_put(static_cast<seL4_CPtr>(held));
}

/* One socket's storage, with nothing behind it yet: the PCB is the caller's to
 * set (a fresh one for `socket`, the accepted one for a connection). Reaches
 * the socket list only when `list_add` gives it an id. */
Socket *alloc_socket(uint32_t kind, uint64_t owner) noexcept
{
    auto *const socket = static_cast<Socket *>(mem_malloc(sizeof(Socket)));
    if (socket == nullptr) {
        return nullptr;
    }
    socket->kind = kind;
    socket->owner = owner;
    socket->raw = nullptr;
    socket->udp = nullptr;
    socket->tcp = nullptr;
    socket->held = 0;
    socket->held_kind = kHoldNone;
    socket->pending_head = nullptr;
    socket->pending_tail = nullptr;
    socket->accept_head = nullptr;
    socket->accept_tail = nullptr;
    socket->next = nullptr;
    return socket;
}

/* A stream segment: copy it to the waiting `recv`, or queue it, and mark the
 * window as read so the peer keeps sending. The pbuf is lwIP's, freed here. A
 * null pbuf is the peer's close -- end-of-stream, a zero length. Runs in the
 * tcpip thread. */
err_t tcp_received(void *argument, struct tcp_pcb *pcb, struct pbuf *packet,
                   err_t err) noexcept
{
    auto *const socket = static_cast<Socket *>(argument);
    if (packet == nullptr) {
        deliver(socket, 0, 0, nullptr, 0, 0);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(packet);
        return ERR_OK;
    }
    deliver_stream(socket, packet, packet->tot_len);
    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);
    return ERR_OK;
}

/* The connect completed (err OK) or failed. Runs in the tcpip thread. */
err_t tcp_connected(void *argument, struct tcp_pcb *pcb, err_t err) noexcept
{
    static_cast<void>(pcb);
    answer_flag(static_cast<Socket *>(argument), kHoldConnect, err == ERR_OK ? 1 : 0);
    return ERR_OK;
}

/* The connection was aborted: the pcb is gone and must not be touched again. A
 * waiting connect failed; a waiting recv sees end-of-stream. Runs in the tcpip
 * thread. */
void tcp_error(void *argument, err_t err) noexcept
{
    static_cast<void>(err);
    auto *const socket = static_cast<Socket *>(argument);
    socket->tcp = nullptr;
    if (__atomic_load_n(&socket->held_kind, __ATOMIC_ACQUIRE) == kHoldConnect) {
        answer_flag(socket, kHoldConnect, 0);
    } else {
        deliver(socket, 0, 0, nullptr, 0, 0);
    }
}

/* A connection arrived at a listener: make it a socket of its own, answer a
 * waiting `accept` with its id, or queue it. Runs in the tcpip thread. */
err_t tcp_on_accept(void *argument, struct tcp_pcb *new_pcb, err_t err) noexcept
{
    auto *const listener = static_cast<Socket *>(argument);
    if (err != ERR_OK || new_pcb == nullptr) {
        return ERR_OK;
    }
    auto *const child = alloc_socket(kStream, listener->owner);
    if (child == nullptr) {
        tcp_abort(new_pcb);
        return ERR_OK;
    }
    child->tcp = new_pcb;
    tcp_arg(new_pcb, child);
    tcp_recv(new_pcb, tcp_received);
    tcp_err(new_pcb, tcp_error);
    list_add(child);
    if (__atomic_load_n(&listener->held_kind, __ATOMIC_ACQUIRE) == kHoldAccept) {
        uintptr_t const held = __atomic_exchange_n(&listener->held, 0, __ATOMIC_ACQ_REL);
        if (held != 0) {
            seL4_SetMR(0, child->id);
            seL4_Send(held, seL4_MessageInfo_new(0, 0, 0, 1));
            slot_put(static_cast<seL4_CPtr>(held));
            return ERR_OK;
        }
    }
    accept_push(listener, child->id);
    return ERR_OK;
}

/* An `accept`, or a connect, that no event answered in time: answer it with 0,
 * and for a connect abort the attempt. Runs in the tcpip thread from lwIP's
 * timers. */
void accept_timeout(void *argument) noexcept
{
    answer_flag(static_cast<Socket *>(argument), kHoldAccept, 0);
}

void connect_timeout(void *argument) noexcept
{
    auto *const socket = static_cast<Socket *>(argument);
    answer_flag(socket, kHoldConnect, 0);
    if (socket->tcp != nullptr) {
        tcp_abort(socket->tcp);
        socket->tcp = nullptr;
    }
}

struct CreateArg {
    Socket *socket;
    bool ok;
};

void do_create(void *argument) noexcept
{
    auto *const create = static_cast<CreateArg *>(argument);
    Socket *const socket = create->socket;
    create->ok = false;
    if (socket->kind == kUdp) {
        socket->udp = udp_new();
        if (socket->udp != nullptr) {
            udp_recv(socket->udp, udp_received, socket);
            /* Bind an ephemeral port now, so a server's reply lands on a socket
             * that is already listening for it rather than racing the first
             * send's implicit bind. */
            if (udp_bind(socket->udp, IP_ANY_TYPE, 0) == ERR_OK) {
                create->ok = true;
            }
        }
    } else if (socket->kind == kStream) {
        socket->tcp = tcp_new();
        if (socket->tcp != nullptr) {
            tcp_arg(socket->tcp, socket);
            tcp_recv(socket->tcp, tcp_received);
            tcp_err(socket->tcp, tcp_error);
            create->ok = true;
        }
    } else {
        socket->raw = raw_new(IP_PROTO_ICMP);
        if (socket->raw != nullptr) {
            raw_recv(socket->raw, raw_received, socket);
            create->ok = true;
        }
    }
}

struct SendArg {
    Socket *socket;
    uint64_t const *words; /* [destination, length, payload words...] */
    uint32_t count;
    uint32_t sent;
};

void do_send(void *argument) noexcept
{
    auto *const send = static_cast<SendArg *>(argument);
    send->sent = 0;
    if (send->count < 2) {
        return;
    }
    uint32_t const length = static_cast<uint32_t>(send->words[1]);
    if (length == 0 || length > kMaxPayloadWords * 8) {
        return;
    }
    struct pbuf *const packet = pbuf_alloc(PBUF_IP, static_cast<u16_t>(length), PBUF_RAM);
    if (packet == nullptr) {
        return;
    }
    auto *const payload = static_cast<uint8_t *>(packet->payload);
    if (packet->len >= length) {
        unpack_words(send->words + 2, send->count - 2, payload, length);
    } else {
        /* A chained pbuf: fill it piecewise. */
        uint8_t scratch[kMaxPayloadWords * 8];
        unpack_words(send->words + 2, send->count - 2, scratch, length);
        pbuf_take(packet, scratch, static_cast<u16_t>(length));
    }
    ip_addr_t destination;
    ip_addr_set_ip4_u32(&destination, static_cast<uint32_t>(send->words[0]));
    if (raw_sendto(send->socket->raw, packet, &destination) == ERR_OK) {
        send->sent = length;
    } else {
        pbuf_free(packet);
    }
}

/* A `sendto` on a UDP socket: the destination is the call's, and the payload is
 * the UDP payload -- the stack adds the UDP and IP headers. The pbuf is the
 * stack's on success and ours on failure, the same convention the raw send
 * keeps. Runs in the tcpip thread. */
struct SendToArg {
    Socket *socket;
    uint64_t const *words; /* [address, port, length, payload words...] */
    uint32_t count;
    uint32_t sent;
};

void do_sendto(void *argument) noexcept
{
    auto *const send = static_cast<SendToArg *>(argument);
    send->sent = 0;
    if (send->socket->udp == nullptr || send->count < 3) {
        return;
    }
    uint32_t const address = static_cast<uint32_t>(send->words[0]);
    auto const port = static_cast<u16_t>(send->words[1] & 0xffff);
    uint32_t const length = static_cast<uint32_t>(send->words[2]);
    if (length == 0 || length > kMaxPayloadWords * 8) {
        return;
    }
    struct pbuf *const packet = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(length),
                                           PBUF_RAM);
    if (packet == nullptr) {
        return;
    }
    uint8_t scratch[kMaxPayloadWords * 8];
    unpack_words(send->words + 3, send->count - 3, scratch, length);
    pbuf_take(packet, scratch, static_cast<u16_t>(length));
    ip_addr_t destination;
    ip_addr_set_ip4_u32(&destination, address);
    if (udp_sendto(send->socket->udp, packet, &destination, port) == ERR_OK) {
        send->sent = length;
    } else {
        pbuf_free(packet);
    }
}

struct BindArg {
    Socket *socket;
    uint32_t address;
    uint32_t port;
    bool ok;
};

void do_bind(void *argument) noexcept
{
    auto *const bind = static_cast<BindArg *>(argument);
    bind->ok = false;
    if (bind->socket->tcp == nullptr) {
        return;
    }
    ip_addr_t address;
    ip_addr_set_ip4_u32(&address, bind->address);
    bind->ok = tcp_bind(bind->socket->tcp, &address,
                        static_cast<u16_t>(bind->port)) == ERR_OK;
}

struct ListenArg {
    Socket *socket;
    bool ok;
};

/* `tcp_listen` answers a *new* pcb (a smaller, listening one); the socket takes
 * it and the accept callback is set on it. Runs in the tcpip thread. */
void do_listen(void *argument) noexcept
{
    auto *const listen = static_cast<ListenArg *>(argument);
    listen->ok = false;
    if (listen->socket->tcp == nullptr) {
        return;
    }
    struct tcp_pcb *const listening = tcp_listen(listen->socket->tcp);
    if (listening == nullptr) {
        return;
    }
    listen->socket->tcp = listening;
    tcp_arg(listening, listen->socket);
    tcp_accept(listening, tcp_on_accept);
    listen->ok = true;
}

struct ConnectArg {
    Socket *socket;
    uint32_t address;
    uint32_t port;
    bool ok;
};

void do_connect(void *argument) noexcept
{
    auto *const connect = static_cast<ConnectArg *>(argument);
    connect->ok = false;
    if (connect->socket->tcp == nullptr) {
        return;
    }
    ip_addr_t address;
    ip_addr_set_ip4_u32(&address, connect->address);
    connect->ok = tcp_connect(connect->socket->tcp, &address,
                              static_cast<u16_t>(connect->port),
                              tcp_connected) == ERR_OK;
}

struct WriteArg {
    Socket *socket;
    uint8_t const *bytes;
    uint32_t length;
    uint32_t written;
};

/* A stream `send`: the bytes go into lwIP's send buffer and out. A full buffer
 * refuses the write (a partial send is a later refinement, with `tcp_sent`).
 * Runs in the tcpip thread. */
void do_write(void *argument) noexcept
{
    auto *const write = static_cast<WriteArg *>(argument);
    write->written = 0;
    if (write->socket->tcp == nullptr || write->length == 0) {
        return;
    }
    err_t const err = tcp_write(write->socket->tcp, write->bytes,
                                static_cast<u16_t>(write->length), TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) {
        write->written = write->length;
        (void)tcp_output(write->socket->tcp);
    }
}

struct CloseArg {
    Socket *socket;
};

void do_close(void *argument) noexcept
{
    auto *const close = static_cast<CloseArg *>(argument);
    Socket *const socket = close->socket;
    /* Nothing may call back into a socket the caller is about to free. An armed
     * timeout fires later, and a stream's pcb can live past its own close (a
     * FIN_WAIT continues the handshake), so the timeouts are cancelled and the
     * pcb's callbacks cleared before it is closed. Runs in the tcpip thread, so
     * no callback for this socket can be in flight once this returns. */
    sys_untimeout(recv_timeout, socket);
    sys_untimeout(accept_timeout, socket);
    sys_untimeout(connect_timeout, socket);
    if (socket->raw != nullptr) {
        raw_remove(socket->raw);
        socket->raw = nullptr;
    }
    if (socket->udp != nullptr) {
        udp_remove(socket->udp);
        socket->udp = nullptr;
    }
    if (socket->tcp != nullptr) {
        tcp_arg(socket->tcp, nullptr);
        tcp_accept(socket->tcp, nullptr);
        /* A listener has no recv/err callback to clear, and lwIP refuses to set
         * one on a LISTEN pcb. */
        if (socket->tcp->state != LISTEN) {
            tcp_recv(socket->tcp, nullptr);
            tcp_err(socket->tcp, nullptr);
        }
        (void)tcp_close(socket->tcp);
        socket->tcp = nullptr;
    }
}

struct ResolveArg {
    seL4_CPtr held;
    char *name;
    uint32_t timeout;
    volatile bool answered;
};

void resolve_finish(ResolveArg *resolve) noexcept
{
    mem_free(resolve->name);
    mem_free(resolve);
}

/* One answer each: DNS's callback or the timeout, whichever comes first. Runs in
 * the tcpip thread. */
void resolve_answer(ResolveArg *resolve, uint64_t address) noexcept
{
    if (!__atomic_exchange_n(&resolve->answered, true, __ATOMIC_ACQ_REL)) {
        answer_held(resolve->held, address);
        resolve_finish(resolve);
    }
}

void dns_found(char const *name, ip_addr_t const *address, void *argument) noexcept
{
    static_cast<void>(name);
    resolve_answer(static_cast<ResolveArg *>(argument),
                   address != nullptr ? ip_word(address) : 0);
}

void resolve_timeout(void *argument) noexcept
{
    resolve_answer(static_cast<ResolveArg *>(argument), 0);
}

void do_resolve(void *argument) noexcept
{
    auto *const resolve = static_cast<ResolveArg *>(argument);
    ip_addr_t address;
    err_t const result = dns_gethostbyname(resolve->name, &address, dns_found, resolve);
    if (result == ERR_OK) {
        resolve_answer(resolve, ip_word(&address));
    } else if (result != ERR_INPROGRESS) {
        resolve_answer(resolve, 0);
    } else if (resolve->timeout != 0) {
        sys_timeout(resolve->timeout, resolve_timeout, resolve);
        /* ERR_INPROGRESS: dns_found answers, or the timeout does. */
    }
}

Socket *find_socket(uint32_t id) noexcept
{
    lock_list();
    Socket *found = nullptr;
    for (Socket *socket = g_sockets; socket != nullptr; socket = socket->next) {
        if (socket->id == id) {
            found = socket;
            break;
        }
    }
    unlock_list();
    return found;
}

uint32_t make_socket(uint64_t domain, uint64_t type, uint64_t protocol,
                     uint64_t owner) noexcept
{
    uint32_t kind = kRaw;
    if (domain != kAfInet) {
        return 0;
    }
    if (type == kSockRaw && protocol == kIpprotoIcmp) {
        kind = kRaw;
    } else if (type == kSockDgram && protocol == kIpprotoUdp) {
        kind = kUdp;
    } else if (type == kSockStream && protocol == kIpprotoTcp) {
        kind = kStream;
    } else {
        return 0;
    }
    Socket *const socket = alloc_socket(kind, owner);
    if (socket == nullptr) {
        return 0;
    }
    CreateArg create{socket, false};
    if (!in_tcpip(do_create, &create) || !create.ok) {
        mem_free(socket);
        return 0;
    }
    list_add(socket);
    return socket->id;
}

/* Tear one already-unlinked socket down: its lwIP state, its queued datagrams,
 * and its storage. Runs on the serve thread; `do_close` runs in the tcpip
 * thread. */
void free_socket(Socket *socket) noexcept
{
    CloseArg close{socket};
    (void)in_tcpip(do_close, &close);
    Datagram *datagram = nullptr;
    while ((datagram = queue_pop(socket)) != nullptr) {
        mem_free(datagram);
    }
    /* Connections accepted but never taken are dropped: their pcbs go with the
     * listener's close. Id 0 is never a socket. */
    while (accept_pop(socket) != 0) {
    }
    mem_free(socket);
}

uint32_t close_socket(uint32_t id) noexcept
{
    lock_list();
    Socket **link = &g_sockets;
    while (*link != nullptr && (*link)->id != id) {
        link = &(*link)->next;
    }
    Socket *const socket = *link;
    if (socket != nullptr) {
        *link = socket->next;
    }
    unlock_list();
    if (socket == nullptr) {
        return 0;
    }
    free_socket(socket);
    return 1;
}

/* Reap every socket the badge owns -- the client died without closing, so the
 * launcher names its badge and this drops what it left. The sockets are unlinked
 * under the list lock first, then torn down outside it, because `free_socket`
 * reaches the tcpip thread and the accept/queue locks and must not hold the list
 * lock. Returns how many were dropped. */
uint32_t reap_owner(uint64_t owner) noexcept
{
    Socket *doomed = nullptr;
    lock_list();
    Socket **link = &g_sockets;
    while (*link != nullptr) {
        Socket *const socket = *link;
        if (socket->owner == owner) {
            *link = socket->next;
            socket->next = doomed;
            doomed = socket;
        } else {
            link = &socket->next;
        }
    }
    unlock_list();
    uint32_t count = 0;
    while (doomed != nullptr) {
        Socket *const socket = doomed;
        doomed = socket->next;
        free_socket(socket);
        ++count;
    }
    if (count != 0) {
        aegir::debug_write("      net: reaped ");
        aegir::debug_write_unsigned(count);
        aegir::debug_write(" socket(s) a client left open\n");
    }
    return count;
}

uint32_t send_on(uint32_t id, uint64_t const *words, uint32_t count) noexcept
{
    Socket *const socket = find_socket(id);
    if (socket == nullptr || socket->raw == nullptr) {
        return 0;
    }
    SendArg send{socket, words, count, 0};
    if (!in_tcpip(do_send, &send)) {
        return 0;
    }
    return send.sent;
}

uint32_t sendto_on(uint32_t id, uint64_t const *words, uint32_t count) noexcept
{
    Socket *const socket = find_socket(id);
    if (socket == nullptr || socket->udp == nullptr) {
        return 0;
    }
    SendToArg send{socket, words, count, 0};
    if (!in_tcpip(do_sendto, &send)) {
        return 0;
    }
    return send.sent;
}

uint32_t bind_on(uint32_t id, uint32_t address, uint32_t port) noexcept
{
    Socket *const socket = find_socket(id);
    if (socket == nullptr || socket->tcp == nullptr) {
        return 0;
    }
    BindArg bind{socket, address, port, false};
    if (!in_tcpip(do_bind, &bind)) {
        return 0;
    }
    return bind.ok ? 1 : 0;
}

uint32_t listen_on(uint32_t id) noexcept
{
    Socket *const socket = find_socket(id);
    if (socket == nullptr || socket->tcp == nullptr) {
        return 0;
    }
    ListenArg listen{socket, false};
    if (!in_tcpip(do_listen, &listen)) {
        return 0;
    }
    return listen.ok ? 1 : 0;
}

uint32_t write_on(uint32_t id, uint8_t const *bytes, uint32_t length) noexcept
{
    Socket *const socket = find_socket(id);
    if (socket == nullptr || socket->tcp == nullptr) {
        return 0;
    }
    WriteArg write{socket, bytes, length, 0};
    if (!in_tcpip(do_write, &write)) {
        return 0;
    }
    return write.written;
}

/* Does a datagram wait? If so, fill the answer words and free it. The shape
 * follows the socket's kind: a datagram's answer carries its peer's port, a raw
 * message's does not. */
bool take_pending(Socket *socket, uint64_t *words, uint32_t *count, bool with_port) noexcept
{
    Datagram *const datagram = queue_pop(socket);
    if (datagram == nullptr) {
        return false;
    }
    if (with_port) {
        words[0] = datagram->source;
        words[1] = datagram->source_port;
        words[2] = datagram->length;
        *count = 3 + pack_bytes(datagram->payload, datagram->length, words + 3);
    } else {
        words[0] = datagram->source;
        words[1] = datagram->length;
        *count = 2 + pack_bytes(datagram->payload, datagram->length, words + 2);
    }
    mem_free(datagram);
    return true;
}

/* A `recv`/`recvfrom`: answer with a datagram already waiting, or hold the
 * caller's reply cap until one arrives (or the timeout passes). The reply cap
 * is saved in the shared CSpace so the tcpip thread can answer it -- the shape
 * specs/signal.md sets for a held reply. `with_port` is the datagram layout. */
void serve_recv(ipc::Owner &port, Socket *socket, uint32_t timeout, bool with_port,
                uint64_t *words) noexcept
{
    seL4_CPtr const root = aegir::bootstrap::kSlotOwnCNode;
    seL4_Word const depth = aegir::bootstrap::kCNodeBits;
    uint32_t count = 0;
    if (take_pending(socket, words, &count, with_port)) {
        port.reply_words(words, count);
        return;
    }
    seL4_CPtr const slot = slot_get();
    if (slot == 0 || seL4_CNode_SaveCaller(root, slot, depth) != seL4_NoError) {
        if (slot != 0) {
            slot_put(slot);
        }
        port.reply(0);
        return;
    }
    __atomic_store_n(&socket->held_kind, kHoldRecv, __ATOMIC_RELEASE);
    __atomic_store_n(&socket->held, static_cast<uintptr_t>(slot), __ATOMIC_RELEASE);
    /* A datagram may have arrived in the gap; take it back. */
    if (take_pending(socket, words, &count, with_port)) {
        uintptr_t const taken = __atomic_exchange_n(&socket->held, 0, __ATOMIC_ACQ_REL);
        if (taken != 0) {
            slot_put(static_cast<seL4_CPtr>(taken));
        }
        port.reply_words(words, count);
    } else if (timeout != 0) {
        /* The client asked not to wait forever: lwIP's timers answer the reply
         * with a zero length if nothing comes. */
        sys_timeout(timeout, recv_timeout, socket);
    }
    /* else: held; the receive path answers it. */
}

/* An `accept`: answer a connection already queued, or hold the caller's reply
 * cap until one arrives (or the timeout passes). */
void serve_accept(ipc::Owner &port, Socket *socket, uint32_t timeout) noexcept
{
    uint32_t const ready = accept_pop(socket);
    if (ready != 0) {
        port.reply(ready);
        return;
    }
    seL4_CPtr const root = aegir::bootstrap::kSlotOwnCNode;
    seL4_Word const depth = aegir::bootstrap::kCNodeBits;
    seL4_CPtr const slot = slot_get();
    if (slot == 0 || seL4_CNode_SaveCaller(root, slot, depth) != seL4_NoError) {
        if (slot != 0) {
            slot_put(slot);
        }
        port.reply(0);
        return;
    }
    __atomic_store_n(&socket->held_kind, kHoldAccept, __ATOMIC_RELEASE);
    __atomic_store_n(&socket->held, static_cast<uintptr_t>(slot), __ATOMIC_RELEASE);
    /* A connection may have arrived in the gap; take it back. */
    uint32_t const now = accept_pop(socket);
    if (now != 0) {
        uintptr_t const taken = __atomic_exchange_n(&socket->held, 0, __ATOMIC_ACQ_REL);
        if (taken != 0) {
            slot_put(static_cast<seL4_CPtr>(taken));
        }
        port.reply(now);
    } else if (timeout != 0) {
        sys_timeout(timeout, accept_timeout, socket);
    }
}

/* A `connect`: hold the caller's reply cap, then start the handshake. The
 * connected callback (or the error callback) answers it -- and the hold is
 * taken *before* the handshake starts, so the answer cannot race it. */
void serve_connect(ipc::Owner &port, Socket *socket, uint32_t address, uint32_t port_number,
                   uint32_t timeout) noexcept
{
    seL4_CPtr const root = aegir::bootstrap::kSlotOwnCNode;
    seL4_Word const depth = aegir::bootstrap::kCNodeBits;
    seL4_CPtr const slot = slot_get();
    if (slot == 0 || seL4_CNode_SaveCaller(root, slot, depth) != seL4_NoError) {
        if (slot != 0) {
            slot_put(slot);
        }
        port.reply(0);
        return;
    }
    __atomic_store_n(&socket->held_kind, kHoldConnect, __ATOMIC_RELEASE);
    __atomic_store_n(&socket->held, static_cast<uintptr_t>(slot), __ATOMIC_RELEASE);
    ConnectArg connect{socket, address, port_number, false};
    if (!in_tcpip(do_connect, &connect) || !connect.ok) {
        uintptr_t const taken = __atomic_exchange_n(&socket->held, 0, __ATOMIC_ACQ_REL);
        if (taken != 0) {
            slot_put(static_cast<seL4_CPtr>(taken));
        }
        port.reply(0);
        return;
    }
    if (timeout != 0) {
        sys_timeout(timeout, connect_timeout, socket);
    }
}

}  // namespace

[[noreturn]] void serve_sockets(void *authority) noexcept
{
    auto const *const auth = static_cast<Authority const *>(authority);
    g_objects = &auth->objects;
    g_account = &auth->account;

    ipc::Owner port =
        ipc::Owner::find(aegir::net::kPortName, aegir::net::kPortNameLength);
    if (!port.valid()) {
        aegir::debug_write("      net: no socket port was given to me\n");
        aegir::halt();
    }
    seL4_CPtr const root = aegir::bootstrap::kSlotOwnCNode;
    seL4_Word const depth = aegir::bootstrap::kCNodeBits;

    for (;;) {
        uint64_t words[kMaxPayloadWords + 4] = {0};
        uint32_t count = 0;
        seL4_Word badge = 0;
        uint32_t const method = port.receive_words(words, kMaxPayloadWords + 4, &count, &badge);
        if (method == aegir::net::kMethodSocket && count >= 3) {
            port.reply(make_socket(words[0], words[1], words[2], badge));
        } else if (method == aegir::net::kMethodClose && count >= 1) {
            port.reply(close_socket(static_cast<uint32_t>(words[0])));
        } else if (method == aegir::net::kMethodReap && count >= 1) {
            port.reply(reap_owner(words[0]));
        } else if (method == aegir::net::kMethodSend && count >= 2) {
            port.reply(send_on(static_cast<uint32_t>(words[0]), words + 1, count - 1));
        } else if (method == aegir::net::kMethodSendTo && count >= 4) {
            port.reply(sendto_on(static_cast<uint32_t>(words[0]), words + 1, count - 1));
        } else if (method == aegir::net::kMethodWrite && count >= 2) {
            uint32_t const length = static_cast<uint32_t>(words[1]);
            if (length > kEnvelopeBytes || length > (count - 2) * 8) {
                port.reply(0);
            } else {
                uint8_t bytes[kEnvelopeBytes];
                unpack_words(words + 2, count - 2, bytes, length);
                port.reply(write_on(static_cast<uint32_t>(words[0]), bytes, length));
            }
        } else if (method == aegir::net::kMethodBind && count >= 3) {
            port.reply(bind_on(static_cast<uint32_t>(words[0]),
                               static_cast<uint32_t>(words[1]),
                               static_cast<uint32_t>(words[2])));
        } else if (method == aegir::net::kMethodListen && count >= 1) {
            port.reply(listen_on(static_cast<uint32_t>(words[0])));
        } else if (method == aegir::net::kMethodAccept && count >= 1) {
            Socket *const socket = find_socket(static_cast<uint32_t>(words[0]));
            uint32_t const timeout = count >= 2 ? static_cast<uint32_t>(words[1]) : 0;
            if (socket == nullptr || socket->kind != kStream) {
                port.reply(0);
            } else {
                serve_accept(port, socket, timeout);
            }
        } else if (method == aegir::net::kMethodConnect && count >= 3) {
            Socket *const socket = find_socket(static_cast<uint32_t>(words[0]));
            uint32_t const timeout = count >= 4 ? static_cast<uint32_t>(words[3]) : 0;
            if (socket == nullptr || socket->kind != kStream) {
                port.reply(0);
            } else {
                serve_connect(port, socket, static_cast<uint32_t>(words[1]),
                              static_cast<uint32_t>(words[2]), timeout);
            }
        } else if ((method == aegir::net::kMethodRecv ||
                    method == aegir::net::kMethodRecvFrom) &&
                   count >= 1) {
            Socket *const socket = find_socket(static_cast<uint32_t>(words[0]));
            uint32_t const timeout = count >= 2 ? static_cast<uint32_t>(words[1]) : 0;
            if (socket == nullptr) {
                port.reply(0);
            } else {
                serve_recv(port, socket, timeout,
                           method == aegir::net::kMethodRecvFrom, words);
            }
        } else if (method == aegir::net::kMethodResolve && count >= 2) {
            uint32_t const name_length = static_cast<uint32_t>(words[0]);
            uint32_t const timeout = count >= 2 ? static_cast<uint32_t>(words[1]) : 0;
            seL4_CPtr const slot = slot_get();
            auto *const resolve = static_cast<ResolveArg *>(mem_malloc(sizeof(ResolveArg)));
            char *const name =
                name_length <= kMaxNameBytes
                    ? static_cast<char *>(mem_malloc(name_length + 1))
                    : nullptr;
            if (slot == 0 || resolve == nullptr || name == nullptr ||
                seL4_CNode_SaveCaller(root, slot, depth) != seL4_NoError) {
                if (slot != 0) {
                    slot_put(slot);
                }
                mem_free(resolve);
                mem_free(name);
                port.reply(0);
            } else {
                unpack_words(words + 2, count - 2, reinterpret_cast<uint8_t *>(name),
                             name_length);
                name[name_length] = '\0';
                resolve->held = slot;
                resolve->name = name;
                resolve->timeout = timeout;
                resolve->answered = false;
                if (!in_tcpip(do_resolve, resolve)) {
                    resolve_answer(resolve, 0);
                }
            }
        } else {
            port.reply(0);
        }
    }
}

}  // namespace aegir::net
