/*
 * aegir-net's link half: a netif and a receive thread for each bound Ethernet
 * link, and the control over them (specs/net.md). See interface.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "interface.h"

#include <aegir/debug.h>
#include <aegir/ethernet.h>
#include <aegir/ipc/port.h>
#include <aegir/netcontrol.h>
#include <aegir/registry.h>
#include <sel4/sel4.h>
#include <stdint.h>

extern "C" {
#include <lwip/dhcp.h>
#include <lwip/etharp.h>
#include <lwip/mem.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>
#include <lwip/tcpip.h>
}

namespace aegir::net {
namespace {

using ethernet::kFrameMax;

/* One link's state: the port it is called through, the window it maps, the
 * netif lwIP routes through, and what the control port describes -- the
 * addresses cached when the tcpip thread learned them, so a reader on another
 * thread does not race lwIP. It comes out of lwIP's heap -- the same growable
 * region as everything else -- and the list grows with it: no fixed table. */
struct Link {
    Link *next;
    unsigned index;
    ipc::Consumer port;
    uint8_t *window;
    uint64_t mac;
    uint16_t mtu;
    bool link_up;
    uint32_t unit;
    uint64_t name; /* "NE0" packed low byte first */
    /* Cached for `describe`, written in the tcpip thread. */
    uint64_t ipv4;
    uint64_t netmask;
    uint64_t gateway;
    uint64_t flags;
    /* One-line diagnostics, printed once each: whether the link has carried a
     * frame in, a frame out, and whether lwIP's timers have run its poll. */
    bool saw_frame;
    bool saw_tx;
    bool saw_poll;
    struct netif netif;
};

Link *g_links = nullptr;
unsigned g_link_count = 0;

Link *link_at(unsigned index) noexcept
{
    for (Link *link = g_links; link != nullptr; link = link->next) {
        if (link->index == index) {
            return link;
        }
    }
    return nullptr;
}

/* "NE" and the link's unit, packed low byte first (the MAC's packing): the
 * adapter's own name, `eth.virtio0` -> `NE0` (specs/net.md). */
uint64_t pack_name(uint32_t unit) noexcept
{
    char text[8];
    uint32_t length = 0;
    text[length++] = 'N';
    text[length++] = 'E';
    char digits[10];
    uint32_t count = 0;
    if (unit == 0) {
        digits[count++] = '0';
    }
    while (unit != 0 && count < 10) {
        digits[count++] = static_cast<char>('0' + (unit % 10));
        unit /= 10;
    }
    while (count > 0) {
        text[length++] = digits[--count];
    }
    uint64_t packed = 0;
    for (uint32_t i = 0; i < length; ++i) {
        packed |= static_cast<uint64_t>(static_cast<uint8_t>(text[i])) << (8 * i);
    }
    return packed;
}

uint32_t bounded_length(char const *text, uint32_t maximum) noexcept
{
    uint32_t length = 0;
    while (length < maximum && text[length] != '\0') {
        ++length;
    }
    return length;
}

bool prefix_is(char const *text, uint32_t length, char const *prefix,
               uint32_t prefix_length) noexcept
{
    if (length < prefix_length) {
        return false;
    }
    for (uint32_t i = 0; i < prefix_length; ++i) {
        if (text[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

/* The unit an instance name ends in: `eth.virtio0` is 0. The adapter name is
 * the link's own unit, the rule that makes `blk.virtio0` BD0 (specs/net.md). */
uint32_t trailing_unit(char const *text, uint32_t length) noexcept
{
    uint32_t unit = 0;
    uint32_t place = 1;
    uint32_t at = length;
    while (at > 0 && text[at - 1] >= '0' && text[at - 1] <= '9') {
        unit += static_cast<uint32_t>(text[at - 1] - '0') * place;
        place *= 10;
        --at;
    }
    return unit;
}

err_t link_output(struct netif *netif, struct pbuf *frame) noexcept
{
    auto *const link = static_cast<Link *>(netif->state);
    u16_t const length = frame->tot_len;
    if (length == 0 || length > kFrameMax) {
        return ERR_VAL;
    }
    pbuf_copy_partial(frame, link->window, length, 0);
    if (!link->saw_tx) {
        link->saw_tx = true;
        aegir::debug_write("      net: NE");
        aegir::debug_write_unsigned(link->unit);
        aegir::debug_write(" first frame out, ");
        aegir::debug_write_unsigned(length);
        aegir::debug_write(" bytes\n");
    }
    ipc::Reply const sent = link->port.call(ethernet::kMethodSend, length);
    return sent.error == 0 && sent.word == length ? ERR_OK : ERR_IF;
}

err_t link_netif_init(struct netif *netif) noexcept
{
    auto *const link = static_cast<Link *>(netif->state);
    netif->name[0] = 'n';
    netif->name[1] = 'e';
    netif->mtu = link->mtu;
    netif->hwaddr_len = ethernet::kMacBytes;
    for (uint32_t i = 0; i < ethernet::kMacBytes; ++i) {
        netif->hwaddr[i] = static_cast<uint8_t>(link->mac >> (8 * i));
    }
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    if (link->link_up) {
        netif->flags |= NETIF_FLAG_LINK_UP;
    }
    netif->output = etharp_output;
    netif->linkoutput = link_output;
    return ERR_OK;
}

/* One thread per link: call `receive` (a held reply, so the caller waits inside
 * the call until a frame arrives), copy the frame into a pbuf, and hand it to
 * lwIP through the netif's input. */
[[noreturn]] void link_receive(void *argument) noexcept
{
    auto *const link = static_cast<Link *>(argument);
    for (;;) {
        ipc::Reply const frame = link->port.call(ethernet::kMethodReceive, 0);
        if (frame.error != 0) {
            aegir::debug_write("      net: a receive call was refused\n");
            aegir::halt();
        }
        uint32_t const length = static_cast<uint32_t>(frame.word);
        if (length == 0 || length > kFrameMax) {
            continue;
        }
        struct pbuf *const frame_pbuf =
            pbuf_alloc(PBUF_RAW, static_cast<u16_t>(length), PBUF_POOL);
        if (frame_pbuf == nullptr) {
            continue;
        }
        pbuf_take(frame_pbuf, link->window + ethernet::kReceiveOffset,
                  static_cast<u16_t>(length));
        if (!link->saw_frame) {
            link->saw_frame = true;
            aegir::debug_write("      net: NE");
            aegir::debug_write_unsigned(link->unit);
            aegir::debug_write(" first frame in, ");
            aegir::debug_write_unsigned(length);
            aegir::debug_write(" bytes\n");
        }
        if (link->netif.input(frame_pbuf, &link->netif) != ERR_OK) {
            pbuf_free(frame_pbuf);
        }
    }
}

/* The DHCP answer arrives asynchronously; lwIP has no bind callback, so a
 * timeout poll checks for it and reports the numbers the network gave -- never
 * a number from the build (specs/net.md). Runs in the tcpip thread. */
void dhcp_poll(void *argument) noexcept
{
    auto *const link = static_cast<Link *>(argument);
    struct netif *const netif = &link->netif;
    if (!link->saw_poll) {
        link->saw_poll = true;
        aegir::debug_write("      net: NE");
        aegir::debug_write_unsigned(link->unit);
        aegir::debug_write(dhcp_supplied_address(netif) ? " dhcp poll: supplied\n"
                                                        : " dhcp poll: not yet\n");
    }
    if (!dhcp_supplied_address(netif)) {
        sys_timeout(500, dhcp_poll, link);
        return;
    }
    ip4_addr_t const *const address = netif_ip4_addr(netif);
    ip4_addr_t const *const netmask = netif_ip4_netmask(netif);
    ip4_addr_t const *const gateway = netif_ip4_gw(netif);
    link->ipv4 = address->addr;
    link->netmask = netmask->addr;
    link->gateway = gateway->addr;
    link->flags |= netcontrol::kStateUp | netcontrol::kStateDhcp;
    aegir::debug_write("      net: NE");
    aegir::debug_write_unsigned(link->unit);
    aegir::debug_write(" ipv4 ");
    aegir::debug_write(ip4addr_ntoa(address));
    aegir::debug_write(" netmask ");
    aegir::debug_write(ip4addr_ntoa(netmask));
    aegir::debug_write(" gateway ");
    aegir::debug_write(ip4addr_ntoa(gateway));
    aegir::debug_write("\n");
}

struct Configure {
    struct netif *netif;
    uint32_t parameter;
    uint64_t value;
    bool taken;
    sys_sem_t done;
};

/* A `set`, applied in the tcpip thread (every netif operation belongs there).
 * `tcpip_callback` posts the work; the call's own semaphore, signalled when the
 * work finishes, makes the caller wait for it. lwIP's own timeout then runs the
 * DHCP poll, and the answer is reported when it arrives rather than the setter
 * waiting for it. */
void apply_configure(void *argument) noexcept
{
    auto *const job = static_cast<Configure *>(argument);
    struct netif *const netif = job->netif;
    switch (job->parameter) {
    case netcontrol::kParamDhcp:
        if (job->value != 0) {
            netif_set_up(netif);
            if (dhcp_start(netif) == ERR_OK) {
                job->taken = true;
                sys_timeout(500, dhcp_poll, netif->state);
                static bool announced = false;
                if (!announced) {
                    announced = true;
                    aegir::debug_write("      net: the dhcp poll is armed\n");
                }
            }
        } else {
            dhcp_stop(netif);
            netif_set_down(netif);
            job->taken = true;
        }
        break;
    case netcontrol::kParamUp:
        if (job->value != 0) {
            netif_set_up(netif);
        } else {
            netif_set_down(netif);
        }
        job->taken = true;
        break;
    default:
        job->taken = false;
        break;
    }
    sys_sem_signal(&job->done);
}

void write_hex_byte(uint8_t value) noexcept
{
    char const digits[] = "0123456789abcdef";
    char pair[2] = {digits[(value >> 4) & 0xf], digits[value & 0xf]};
    aegir::debug_write(pair, 2);
}

}  // namespace

unsigned add_links(Authority const &authority) noexcept
{
    ipc::Consumer const registry =
        ipc::Consumer::find(registry::kPortName, registry::kPortNameLength);
    if (!registry.valid()) {
        aegir::debug_write("      net: devmgr.registry was not given\n");
        return 0;
    }
    ipc::Reply const count = registry.call(registry::kMethodCount, 0);
    if (count.error != 0) {
        return 0;
    }
    unsigned added = 0;
    for (uint64_t index = 0; index < count.word; ++index) {
        uint64_t words[registry::kRowWords];
        ipc::WordsReply const described = registry.call_words(
            registry::kMethodDescribe, &index, 1, words, registry::kRowWords);
        if (described.error != 0 || described.count != registry::kRowWords) {
            continue;
        }
        auto const *const row = reinterpret_cast<registry::Row const *>(words);
        if (row->bound == 0) {
            continue;
        }
        uint32_t const name_length = bounded_length(row->instance, sizeof(row->instance));
        if (!prefix_is(row->instance, name_length, "eth.", 4)) {
            continue; /* a link class the stack does not speak */
        }

        seL4_CPtr const port_slot = authority.objects.alloc_slot();
        if (port_slot == 0 ||
            !registry::open_bound(registry, row->instance, name_length, port_slot)) {
            aegir::debug_write("      net: the link would not open\n");
            continue;
        }
        ipc::Consumer const link_port(port_slot);

        /* The link's window: its shape, then each frame mapped into our own
         * window at consecutive addresses, so the frame bytes are contiguous. */
        uint64_t page_bits = 0;
        uint64_t pages = 0;
        if (!registry::window_geometry(registry, index, &page_bits, &pages) || pages == 0 ||
            page_bits != seL4_PageBits) {
            aegir::debug_write("      net: the link's window is not usable\n");
            continue;
        }
        uint8_t *window = nullptr;
        bool mapped = true;
        for (uint64_t frame = 0; frame < pages; ++frame) {
            seL4_CPtr const frame_slot = authority.objects.alloc_slot();
            if (frame_slot == 0 || !registry::window_frame(registry, index, frame, frame_slot)) {
                mapped = false;
                break;
            }
            void *const at = authority.scratch.map(frame_slot);
            if (frame == 0) {
                window = static_cast<uint8_t *>(at);
            }
        }
        if (!mapped || window == nullptr ||
            pages * static_cast<uint64_t>(1u << page_bits) <
                ethernet::kReceiveOffset + kFrameMax) {
            aegir::debug_write("      net: the link's window would not map\n");
            continue;
        }

        uint64_t raw[ethernet::kInfoWords];
        ipc::WordsReply const info = link_port.call_words(
            ethernet::kMethodInfo, nullptr, 0, raw, ethernet::kInfoWords);
        if (info.error != 0 || info.count != ethernet::kInfoWords) {
            aegir::debug_write("      net: the link would not say what it is\n");
            continue;
        }

        auto *const link = static_cast<Link *>(mem_malloc(sizeof(Link)));
        if (link == nullptr) {
            break;
        }
        /* lwIP's netif starts zeroed; the rest of the link is set field by
         * field (Link holds a Consumer, so it is not something to memset). */
        __builtin_memset(&link->netif, 0, sizeof(link->netif));
        link->next = nullptr;
        link->index = added;
        link->port = link_port;
        link->window = window;
        link->mac = raw[0];
        link->mtu = static_cast<uint16_t>(raw[1]);
        link->link_up = (raw[2] & ethernet::kInfoLinkUp) != 0;
        link->unit = trailing_unit(row->instance, name_length);
        link->name = pack_name(link->unit);
        link->ipv4 = 0;
        link->netmask = 0;
        link->gateway = 0;
        link->flags = link->link_up ? netcontrol::kStateLinkUp : 0;
        link->saw_frame = false;
        link->saw_tx = false;
        link->saw_poll = false;

        ip4_addr_t any;
        ip4_addr_set_zero(&any);
        struct netif *const netif = netif_add(&link->netif, &any, &any, &any, link,
                                              link_netif_init, tcpip_input);
        if (netif == nullptr) {
            mem_free(link);
            continue;
        }

        thread::Thread receiver{};
        if (!authority.builder.start(authority.placement, link_receive, link, receiver)) {
            aegir::debug_write("      net: the link's receive thread would not start\n");
            continue;
        }

        link->next = g_links;
        g_links = link;
        ++g_link_count;

        aegir::debug_write("      net: NE");
        aegir::debug_write_unsigned(link->unit);
        aegir::debug_write(" ");
        aegir::debug_write(row->instance, name_length);
        aegir::debug_write(" mac ");
        for (uint32_t b = 0; b < ethernet::kMacBytes; ++b) {
            if (b != 0) {
                aegir::debug_write(":");
            }
            write_hex_byte(static_cast<uint8_t>(link->mac >> (8 * b)));
        }
        aegir::debug_write(", mtu ");
        aegir::debug_write_unsigned(link->mtu);
        aegir::debug_write(", link ");
        aegir::debug_write(link->link_up ? "up" : "down");
        aegir::debug_write(", down\n");
        ++added;
    }
    return added;
}

unsigned link_count() noexcept
{
    return g_link_count;
}

bool link_state(unsigned index, LinkState *state) noexcept
{
    Link *const link = link_at(index);
    if (link == nullptr) {
        return false;
    }
    state->name = link->name;
    state->ipv4 = link->ipv4;
    state->netmask = link->netmask;
    state->gateway = link->gateway;
    state->flags = link->flags;
    return true;
}

bool link_configure(unsigned index, uint32_t parameter, uint64_t value) noexcept
{
    Link *const link = link_at(index);
    if (link == nullptr) {
        return false;
    }
    Configure job{};
    job.netif = &link->netif;
    job.parameter = parameter;
    job.value = value;
    job.taken = false;
    if (sys_sem_new(&job.done, 0) != ERR_OK) {
        return false;
    }
    if (tcpip_callback(apply_configure, &job) != ERR_OK) {
        sys_sem_free(&job.done);
        return false;
    }
    sys_arch_sem_wait(&job.done, 0);
    sys_sem_free(&job.done);
    return job.taken;
}

}  // namespace aegir::net
