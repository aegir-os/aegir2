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
#include <lwip/tcpip.h>
#include <lwip/timeouts.h>
}

namespace aegir::net {
namespace {

/* A received datagram waiting for its socket's next `recv`: the source
 * address, the length, and the bytes. */
struct Datagram {
    uint32_t source;
    uint32_t length;
    Datagram *next;
    uint8_t payload[];
};

/* One open socket: the lwIP raw pcb, the reply slot a `recv` saved (0 when the
 * client is not waiting), and a datagram that arrived with no waiter. `held`
 * and `pending` cross between this thread and the tcpip thread, so they are
 * touched atomically. */
struct Socket {
    uint32_t id;
    struct raw_pcb *pcb;
    volatile uintptr_t held;
    /* Datagrams that arrived with no waiter, oldest first. A raw ICMP socket
     * sees more than one message per exchange -- the looped request and its
     * reply -- so one slot would drop all but the first. */
    Datagram *pending_head;
    Datagram *pending_tail;
    Socket *next;
};

Socket *g_sockets = nullptr;
uint32_t g_next_id = 1;
aegir::mem::Allocator *g_objects = nullptr;
aegir::mem::Account *g_account = nullptr;

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

/* A reply from the link: answer the waiting `recv`, or keep the datagram for
 * the next one. Runs in the tcpip thread. The payload's bytes are the low bytes
 * of the answer's words, so they are copied straight into the word array. */
u8_t raw_received(void *argument, struct raw_pcb *pcb, struct pbuf *packet,
                  ip_addr_t const *source) noexcept
{
    static_cast<void>(pcb);
    auto *const socket = static_cast<Socket *>(argument);
    uint32_t const offset = ip_payload_offset(packet);
    uint32_t length = packet->tot_len - offset;
    if (length > kMaxPayloadWords * 8) {
        length = kMaxPayloadWords * 8;
    }
    uintptr_t const held = __atomic_exchange_n(&socket->held, 0, __ATOMIC_ACQ_REL);
    if (held != 0) {
        /* One write: the datagram is copied straight into the message registers,
         * not into an array and then from the array into the registers. */
        seL4_Word *const message = seL4_GetIPCBuffer()->msg;
        message[0] = ip_word(source);
        message[1] = length;
        pbuf_copy_partial(packet, reinterpret_cast<uint8_t *>(message + 2),
                          static_cast<u16_t>(length), static_cast<u16_t>(offset));
        seL4_Send(held, seL4_MessageInfo_new(0, 0, 0, 2 + (length + 7) / 8));
        slot_put(static_cast<seL4_CPtr>(held));
    } else {
        auto *const datagram =
            static_cast<Datagram *>(mem_malloc(sizeof(Datagram) + length));
        if (datagram != nullptr) {
            datagram->source = ip_word(source);
            datagram->length = length;
            datagram->next = nullptr;
            pbuf_copy_partial(packet, datagram->payload, static_cast<u16_t>(length),
                              static_cast<u16_t>(offset));
            queue_push(socket, datagram);
        }
    }
    /* Not consumed: lwIP's ICMP layer must still see the packet, or an echo
     * request to a local address -- 127.0.0.1, looped back -- is never answered,
     * because raw_input runs first and eating the packet stops the chain. The
     * socket has its copy either way, so ping still sees the request and then
     * the reply. */
    return 0;
}

/* A `recv` no packet answered: answer the held reply with a zero length -- a
 * timeout, which a client reads as end-of-stream -- and give the slot back. Runs
 * in the tcpip thread from lwIP's timers. */
void recv_timeout(void *argument) noexcept
{
    auto *const socket = static_cast<Socket *>(argument);
    uintptr_t const held = __atomic_exchange_n(&socket->held, 0, __ATOMIC_ACQ_REL);
    if (held != 0) {
        seL4_SetMR(0, 0);
        seL4_Send(held, seL4_MessageInfo_new(0, 0, 0, 1));
        slot_put(static_cast<seL4_CPtr>(held));
    }
}

struct CreateArg {
    Socket *socket;
    bool ok;
};

void do_create(void *argument) noexcept
{
    auto *const create = static_cast<CreateArg *>(argument);
    create->socket->pcb = raw_new(IP_PROTO_ICMP);
    if (create->socket->pcb != nullptr) {
        raw_recv(create->socket->pcb, raw_received, create->socket);
    }
    create->ok = create->socket->pcb != nullptr;
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
    if (raw_sendto(send->socket->pcb, packet, &destination) == ERR_OK) {
        send->sent = length;
    } else {
        pbuf_free(packet);
    }
}

struct CloseArg {
    Socket *socket;
};

void do_close(void *argument) noexcept
{
    auto *const close = static_cast<CloseArg *>(argument);
    if (close->socket->pcb != nullptr) {
        raw_remove(close->socket->pcb);
        close->socket->pcb = nullptr;
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
    for (Socket *socket = g_sockets; socket != nullptr; socket = socket->next) {
        if (socket->id == id) {
            return socket;
        }
    }
    return nullptr;
}

uint32_t make_socket(uint64_t domain, uint64_t type, uint64_t protocol) noexcept
{
    if (domain != kAfInet || type != kSockRaw || protocol != kIpprotoIcmp) {
        return 0;
    }
    auto *const socket = static_cast<Socket *>(mem_malloc(sizeof(Socket)));
    if (socket == nullptr) {
        return 0;
    }
    socket->id = g_next_id++;
    socket->pcb = nullptr;
    socket->held = 0;
    socket->pending_head = nullptr;
    socket->pending_tail = nullptr;
    socket->next = g_sockets;
    CreateArg create{socket, false};
    if (!in_tcpip(do_create, &create) || !create.ok) {
        mem_free(socket);
        return 0;
    }
    g_sockets = socket;
    return socket->id;
}

uint32_t close_socket(uint32_t id) noexcept
{
    Socket **link = &g_sockets;
    while (*link != nullptr && (*link)->id != id) {
        link = &(*link)->next;
    }
    Socket *const socket = *link;
    if (socket == nullptr) {
        return 0;
    }
    *link = socket->next;
    CloseArg close{socket};
    (void)in_tcpip(do_close, &close);
    Datagram *datagram = nullptr;
    while ((datagram = queue_pop(socket)) != nullptr) {
        mem_free(datagram);
    }
    mem_free(socket);
    return 1;
}

uint32_t send_on(uint32_t id, uint64_t const *words, uint32_t count) noexcept
{
    Socket *const socket = find_socket(id);
    if (socket == nullptr || socket->pcb == nullptr) {
        return 0;
    }
    SendArg send{socket, words, count, 0};
    if (!in_tcpip(do_send, &send)) {
        return 0;
    }
    return send.sent;
}

/* Does a datagram wait? If so, fill the answer words and free it. */
bool take_pending(Socket *socket, uint64_t *words, uint32_t *count) noexcept
{
    Datagram *const datagram = queue_pop(socket);
    if (datagram == nullptr) {
        return false;
    }
    words[0] = datagram->source;
    words[1] = datagram->length;
    *count = 2 + pack_bytes(datagram->payload, datagram->length, words + 2);
    mem_free(datagram);
    return true;
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
            port.reply(make_socket(words[0], words[1], words[2]));
        } else if (method == aegir::net::kMethodClose && count >= 1) {
            port.reply(close_socket(static_cast<uint32_t>(words[0])));
        } else if (method == aegir::net::kMethodSend && count >= 2) {
            port.reply(send_on(static_cast<uint32_t>(words[0]), words + 1, count - 1));
        } else if (method == aegir::net::kMethodRecv && count >= 1) {
            Socket *const socket = find_socket(static_cast<uint32_t>(words[0]));
            uint32_t const timeout = count >= 2 ? static_cast<uint32_t>(words[1]) : 0;
            if (socket == nullptr) {
                port.reply(0);
            } else if (take_pending(socket, words, &count)) {
                port.reply_words(words, count);
            } else {
                seL4_CPtr const slot = slot_get();
                if (slot == 0 || seL4_CNode_SaveCaller(root, slot, depth) != seL4_NoError) {
                    if (slot != 0) {
                        slot_put(slot);
                    }
                    port.reply(0);
                } else {
                    __atomic_store_n(&socket->held, static_cast<uintptr_t>(slot),
                                     __ATOMIC_RELEASE);
                    /* A reply may have arrived in the gap; take it back. */
                    if (take_pending(socket, words, &count)) {
                        uintptr_t const taken =
                            __atomic_exchange_n(&socket->held, 0, __ATOMIC_ACQ_REL);
                        if (taken != 0) {
                            slot_put(static_cast<seL4_CPtr>(taken));
                        }
                        port.reply_words(words, count);
                    } else if (timeout != 0) {
                        /* The client asked not to wait forever: lwIP's timers
                         * answer the reply with a zero length if no packet
                         * comes. */
                        sys_timeout(timeout, recv_timeout, socket);
                    }
                    /* else: held; the link's receive path answers it. */
                }
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
