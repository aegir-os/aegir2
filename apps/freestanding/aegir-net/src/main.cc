/*
 * aegir-net: the network stack -- lwIP over every Ethernet link.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * It runs lwIP's threaded mode (NO_SYS=0): a tcpip thread owns the protocol
 * code, and the service talks to it the way lwIP's threaded API asks
 * (specs/net.md). This file is the service's bring-up -- the authority it was
 * given turned into an allocator, a heap, a thread, and the timer tick that
 * drives lwIP's timers -- and then the link and socket halves land on top of
 * it. Nothing here names the machine: the MAC, MTU and link state come from the
 * link port (aegir/ethernet.h), the addresses come from DHCP, and checksums are
 * lwIP's, so a board's NIC is a new link driver and this stays as it is.
 *
 * The device manager starts it (specs/net.md), as it starts a driver: an
 * untyped the stack's objects and its heap come out of, its own VSpace root so
 * it can map the link windows it opens through the registry, and the caller
 * halves of devmgr.registry and timer.main.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/lwip/port.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/netcontrol.h>
#include <aegir/registry.h>
#include <aegir/thread.h>
#include <aegir/timer.h>
#include <sel4/sel4.h>
#include <stdint.h>

#include "interface.h"

extern "C" {
#include <lwip/netif.h>
#include <lwip/tcpip.h>
}

namespace {

/* The page size a heap run is carved in; the kernel's own constant, named
 * because the heap source rounds to it. */
constexpr uint32_t kPage = 4096;
/* How many pages the tcpip thread's stack gets. lwIP's protocol code is
 * stack-hungry -- it builds and parses segments on the stack -- so this is
 * generous rather than tight. */
constexpr unsigned kTcpipStackPages = 8;
/* The tick the timer signals, in nanoseconds: lwIP's timers are coarse (its
 * fastest is the 250 ms TCP timer), so a tenth of a second bounds how late one
 * runs without waking the thread more than it needs. */
constexpr uint64_t kTickPeriodNs = 100ull * 1000ull * 1000ull;

void write_line(char const *label, char const *text) noexcept
{
    aegir::debug_write("      ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

/* Say what went wrong, tell our spawner we will not serve, and stop -- so the
 * boot carries on and the line is the failure, rather than a hang with no
 * explanation. */
[[noreturn]] void fail(char const *text) noexcept
{
    write_line("FAIL", text);
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}

/* Static, not local: the allocator carries the tables of what it handed out,
 * and they must outlive every object retyped from them. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);
aegir::mem::Account g_account{"net", 0, 0, 0};

/* lwIP's heap source (aegir/lwip/port.h): retype a run of pages out of the
 * stack's untyped, map them into its own window, and answer the region. lwIP
 * asks for more only when it has no free block big enough, so this is the heap
 * growing on demand -- no MEM_SIZE, and a later source can be mem.main. The run
 * is a power of two pages because one retype fills a consecutive slot run
 * (allocator.h's alloc_pages_run). */
void *heap_source(void *context, uint32_t bytes, uint32_t *got_bytes) noexcept
{
    static_cast<void>(context);
    uint32_t pages = 1;
    while (pages * kPage < bytes) {
        pages <<= 1;
    }
    uintptr_t const base = g_scratch.reserve(pages);
    if (base == 0) {
        return nullptr;
    }
    seL4_Error error = seL4_NoError;
    seL4_CPtr const run = g_objects.alloc_pages_run(pages, g_account, &error);
    if (run == 0) {
        return nullptr;
    }
    for (uint32_t i = 0; i < pages; ++i) {
        if (!g_scratch.map_at(base + i * kPage, run + i)) {
            return nullptr;
        }
    }
    *got_bytes = pages * kPage;
    return reinterpret_cast<void *>(base);
}

/* Where the process's own free slots begin: everything past the capabilities
 * the bootstrap block named. The allocator hands them out; the first is where a
 * capability transferred to us would land. The timer keeps the same rule. */
uint64_t first_free_slot() noexcept
{
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            uint64_t const occupied =
                entry.kind == aegir::bootstrap::EntryKind::Capability ? entry.number
                : entry.kind == aegir::bootstrap::EntryKind::DeviceCapability
                    ? entry.reserved
                    : 0;
            if (occupied != 0 && occupied + 1 > first_free) {
                first_free = occupied + 1;
            }
        }
    }
    return first_free;
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

    /* The authority: the untyped our objects and heap come out of, and our own
     * VSpace root and the window of free addresses we map through. Without all
     * three the stack cannot run. */
    uint64_t untyped_slot = 0;
    uint32_t untyped_bits = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    if (!aegir::bootstrap::capability("untyped", 7, &untyped_slot) ||
        !aegir::bootstrap::capability_size_bits("untyped", 7, &untyped_bits) ||
        !aegir::bootstrap::capability("vspace", 6, &vspace_slot) ||
        !aegir::bootstrap::window(&window_base, &window_bytes)) {
        fail("no untyped, vspace or window was given to me");
    }
    if (!g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits)) {
        fail("the untyped would not be remembered");
    }
    uint64_t const first_free = first_free_slot();
    /* The depth is zero because these are our own slots: at depth zero the
     * destination of a retype is the CNode (kernel/src/object/untyped.c). */
    g_objects.adopt_slots(static_cast<seL4_CPtr>(first_free),
                          (1u << aegir::bootstrap::kCNodeBits) -
                              static_cast<uint32_t>(first_free),
                          0, aegir::bootstrap::kCNodeBits);
    if (!g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                         static_cast<uintptr_t>(window_base),
                         static_cast<uintptr_t>(window_base + window_bytes), &g_objects)) {
        fail("the window would not be adopted");
    }
    seL4_SetCapReceivePath(aegir::bootstrap::kSlotOwnCNode,
                           static_cast<seL4_CPtr>(first_free),
                           aegir::bootstrap::kCNodeBits);

    /* The port's side of lwIP: the heap it grows from, the allocator its
     * blocking objects come out of, and the thread builder the tcpip thread is
     * made with. */
    aegir::lwip::set_heap_source(heap_source, nullptr);
    aegir::lwip::set_objects(g_objects, g_account);
    static aegir::thread::Builder builder(g_objects, g_scratch, g_account);
    aegir::thread::Placement const where{
        aegir::bootstrap::kSlotOwnCNode,
        static_cast<seL4_CPtr>(vspace_slot),
        seL4_CapNull,
        seL4_MaxPrio - 1,
        kTcpipStackPages,
    };
    aegir::lwip::set_threading(builder, where);
    /* The tick's period, so lwIP's timers advance: the sys_arch adds it to the
     * clock each time the tick wakes the tcpip thread. */
    aegir::lwip::set_tick_milliseconds(static_cast<uint32_t>(kTickPeriodNs / 1000000ull));

    /* The tick: a notification the timer signals each period. It is minted with
     * a high context bit so the tcpip thread's timed wait can tell a tick from
     * a mailbox wake, and with send rights because the timer must signal it
     * (specs/timer.md). */
    seL4_Error notify_error = seL4_NoError;
    seL4_CPtr const tick_object = g_objects.alloc_object(
        seL4_NotificationObject, seL4_NotificationBits, g_account, &notify_error);
    seL4_CPtr const tick_cap = tick_object != 0 ? g_objects.alloc_slot() : 0;
    if (tick_object == 0 || tick_cap == 0 ||
        seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, tick_cap,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        tick_object, aegir::bootstrap::kCNodeBits,
                        seL4_CapRights_new(0, 0, 1, 1),
                        aegir::lwip::kTickBit) != seL4_NoError) {
        fail("the tick notification could not be made");
    }

    /* Start lwIP: lwip_init, the tcpip mailbox, and the tcpip thread -- which is
     * where the sys_arch, the thread builder and the heap are all exercised for
     * the first time. */
    tcpip_init(nullptr, nullptr);
    seL4_CPtr const tcpip = aegir::lwip::thread_tcb();
    if (tcpip == 0) {
        fail("the tcpip thread was not started");
    }
    /* The loopback interface: lwIP's netif_init added it and brought it up, so
     * the stack has one interface from the first moment, before any NIC is
     * bound -- 127.0.0.1 is always reachable. It is read back, not assumed. */
    struct netif *const loopback = netif_find("lo0");
    if (loopback == nullptr || !netif_is_up(loopback) || !netif_is_link_up(loopback)) {
        fail("the loopback interface is not up");
    }
    write_line("net", "loopback lo0 127.0.0.1/8 up");

    /* The links: every bound eth.* row the registry knows, each opened, its
     * window mapped into ours, and a netif and a receive thread added. An
     * interface starts down -- only a configuration brings it up (specs/net.md)
     * -- with the device's own MAC, MTU and link state. */
    aegir::net::Authority const authority{g_objects, g_scratch, g_account, builder,
                                          where};
    unsigned const links = aegir::net::add_links(authority);
    aegir::debug_write("      net: ");
    aegir::debug_write_unsigned(links);
    aegir::debug_write(links == 1 ? " link\n" : " links\n");

    /* Bind the tick to the tcpip thread, so its timed mailbox fetch wakes on the
     * timer as well as on lwIP's mailbox. */
    if (seL4_TCB_BindNotification(tcpip, tick_cap) != seL4_NoError) {
        fail("the tick would not bind to the tcpip thread");
    }

    /* Subscribe that tick with the timer: the timer sips a copy of the cap and
     * signals it each period. The subscription lives for this process. */
    aegir::ipc::Consumer const timer =
        aegir::ipc::Consumer::find(aegir::timer::kPortName, aegir::timer::kPortNameLength);
    if (!timer.valid()) {
        fail("no timer port was given to me");
    }
    uint64_t const period[1] = {kTickPeriodNs};
    uint64_t answer[1] = {0};
    bool cap_arrived = false;
    aegir::ipc::WordsReply const subscribed = timer.call_transfer(
        aegir::timer::kMethodSubscribe, period, 1, tick_cap, answer, 1, &cap_arrived);
    if (subscribed.error != 0 || answer[0] == 0) {
        fail("the timer refused the tick");
    }

    /* Ready, then serve the control port -- how an adapter is listed and
     * configured, apart from the socket port (specs/net.md). It is served on
     * this thread while the tcpip thread runs lwIP; the tick belongs to the
     * tcpip thread, so a wake here is always a call. */
    aegir::ipc::Owner control = aegir::ipc::Owner::find(
        aegir::netcontrol::kPortName, aegir::netcontrol::kPortNameLength);
    if (!control.valid()) {
        fail("no control port was given to me");
    }
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("net", "ready: the stack is up");
    for (;;) {
        uint64_t words[4] = {0, 0, 0, 0};
        uint32_t count = 0;
        seL4_Word badge = 0;
        uint32_t const method = control.receive_words(words, 4, &count, &badge);
        if (method == aegir::netcontrol::kMethodList) {
            control.reply(aegir::net::link_count());
        } else if (method == aegir::netcontrol::kMethodDescribe && count >= 1) {
            aegir::net::LinkState state{};
            if (aegir::net::link_state(static_cast<unsigned>(words[0]), &state)) {
                uint64_t const answer[aegir::netcontrol::kDescribeWords] = {
                    state.name, state.ipv4, state.netmask, state.gateway, state.flags};
                control.reply_words(answer, aegir::netcontrol::kDescribeWords);
            } else {
                control.reply(0);
            }
        } else if (method == aegir::netcontrol::kMethodSet && count >= 3) {
            bool const taken = aegir::net::link_configure(
                static_cast<unsigned>(words[0]), static_cast<uint32_t>(words[1]), words[2]);
            control.reply(taken ? 1 : 0);
        } else {
            /* A method we do not know is a protocol version we do not speak. */
            control.reply(0);
        }
    }
}
