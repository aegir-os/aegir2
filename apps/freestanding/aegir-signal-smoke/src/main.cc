/*
 * aegir-signal-smoke: the signal primitive's acceptance pair (specs/signal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One binary, two manifest names, because a held reply needs a caller blocked
 * in a call and one process cannot hold its own: `signal.smoke` serves a port
 * with a Receiver and a Reply_holder, and `signal.client` is its caller. The
 * server makes its notification the way a service with no delegated untyped
 * must -- retyped from a chunk mem.main hands it (specs/memory.md) -- which is
 * Receiver::adopt's first user.
 *
 * What the client proves, in order:
 *   capability  the server mints a context for it and sends the capability;
 *   signal      it signals that context, and the server's serving receive sees
 *               the bit (is_signal, ready);
 *   held        the server finds nothing ready, saves its reply, signals its
 *               own second context, and answers from the notification branch.
 *
 * A failure prints SIGNAL_SMOKE_FAIL, which the harness counts (the guest's
 * own verdict, scripts/run_target.py).
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/memory.h>
#include <aegir/signal.h>

#include <sel4/sel4.h>

namespace {

constexpr uint32_t kMethodCapability = 1;
constexpr uint32_t kMethodSignal = 2;
constexpr uint32_t kMethodHeld = 3;

constexpr char const kPortName[] = "signal.smoke";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

void write(char const *text) noexcept
{
    aegir::debug_write(text);
}

void fail(char const *what) noexcept
{
    write("  signal-smoke: SIGNAL_SMOKE_FAIL ");
    write(what);
    write("\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}

/* The first slot past everything the bootstrap block named: this process's
 * own, the way every smoke's are. */
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

/* A chunk from mem.main, moved into `slot`: what a service with no delegated
 * untyped makes its objects from. */
bool chunk_into(seL4_CPtr slot) noexcept
{
    aegir::ipc::Consumer const mem = aegir::ipc::Consumer::find(
        aegir::memory::kPortName, aegir::memory::kPortNameLength);
    if (!mem.valid()) {
        return false;
    }
    uint64_t const request = aegir::memory::kChunkBits;
    uint64_t answer[1] = {};
    bool cap_arrived = false;
    aegir::ipc::WordsReply const alloc =
        mem.call_transfer(aegir::memory::kMethodAlloc, &request, 1, 0, answer, 1,
                          &cap_arrived);
    return alloc.error == 0 && cap_arrived && aegir::ipc::take_received_cap(slot);
}

int serve() noexcept
{
    uint64_t port_slot = 0;
    if (!aegir::bootstrap::capability(kPortName, kPortNameLength, &port_slot)) {
        fail("the port was not given");
    }
    seL4_CPtr const port_cap = static_cast<seL4_CPtr>(port_slot);
    aegir::ipc::Owner port(port_cap);

    uint64_t const first_free = first_free_slot();
    seL4_CPtr const chunk = static_cast<seL4_CPtr>(first_free);
    seL4_CPtr const notification = static_cast<seL4_CPtr>(first_free + 1);
    seL4_CPtr const self_mint = static_cast<seL4_CPtr>(first_free + 2);
    seL4_CPtr const held_slot = static_cast<seL4_CPtr>(first_free + 3);
    seL4_CPtr const client_mint = static_cast<seL4_CPtr>(first_free + 4);

    if (!chunk_into(chunk)) {
        fail("mem.main gave no chunk");
    }
    /* The notification, retyped from the chunk. A service with no delegated
     * untyped still has this one, and Receiver::adopt takes it. */
    if (seL4_Untyped_Retype(chunk, seL4_NotificationObject, seL4_NotificationBits,
                            aegir::bootstrap::kSlotOwnCNode,
                            aegir::bootstrap::kSlotOwnCNode, 0, notification,
                            1) != seL4_NoError) {
        fail("the chunk would not retype a notification");
    }
    aegir::signal::Receiver wake;
    if (!wake.adopt(notification, aegir::bootstrap::kSlotOwnTcb,
                    aegir::bootstrap::kSlotOwnCNode, aegir::bootstrap::cnode_bits())) {
        fail("the notification would not bind");
    }
    aegir::signal::Context const client = wake.create();
    aegir::signal::Context const self = wake.create();
    if (!client.valid() || !self.valid()) {
        fail("the contexts would not be made");
    }
    aegir::signal::Context_capability const self_cap = wake.mint(self, self_mint);
    if (!self_cap.valid()) {
        fail("the self context would not mint");
    }
    aegir::signal::Reply_holder held(aegir::bootstrap::kSlotOwnCNode,
                                     aegir::bootstrap::cnode_bits(), held_slot);

    /* Ready to serve: the supervisor counts a service from this signal, and a
     * server that never sends it is a boot that never completes. */
    write("  signal-smoke: ready, serving signal.smoke\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    uint64_t seen = 0;
    for (;;) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t const info = seL4_Recv(port_cap, &badge);
        if (wake.is_signal(badge)) {
            /* A wakeup, not a call: the message registers keep whatever the
             * last call left, so the badge is the only thing to read. */
            seen |= badge;
            /* A held caller whose context fired is answered from here -- the
             * shape an interrupt-driven service uses. */
            if (held.held() && wake.ready(badge, self)) {
                uint64_t const one = 1;
                held.reply(&one, 1, 0);
            }
            continue;
        }
        if (seL4_MessageInfo_get_length(info) < 1) {
            port.reply(0);
            continue;
        }
        uint32_t const method = static_cast<uint32_t>(seL4_GetMR(0));
        if (method == kMethodCapability) {
            /* One mint per call, deleted after it crosses: the reply transfers
             * a copy and the slot stays ours. */
            aegir::signal::Context_capability const cap = wake.mint(client, client_mint);
            if (cap.valid()) {
                port.reply_cap(nullptr, 0, client_mint);
                seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, client_mint,
                                  aegir::bootstrap::cnode_bits());
            } else {
                write("  signal-smoke: SIGNAL_SMOKE_FAIL the client's mint failed\n");
                port.reply(0);
            }
        } else if (method == kMethodSignal) {
            port.reply((seen & client.badge()) != 0 ? 1 : 0);
        } else if (method == kMethodHeld) {
            if (held.held() || !held.save()) {
                /* One waiter at a time: a second while one is held, or a call
                 * with no caller to hold, is refused rather than lost. */
                port.reply(0);
            } else {
                /* Nothing was ready, so the reply is held and the world is
                 * moved -- a device would do this from its interrupt. */
                aegir::signal::Transmitter::signal(self_cap);
            }
        } else {
            /* A method we do not know is a protocol version we do not speak:
             * the reply says so by saying nothing. */
            port.reply_words(nullptr, 0);
        }
    }
}

int run_client() noexcept
{
    aegir::ipc::Consumer const port =
        aegir::ipc::Consumer::find(kPortName, kPortNameLength);
    if (!port.valid()) {
        fail("no signal.smoke port to call");
    }
    seL4_CPtr const mint = static_cast<seL4_CPtr>(first_free_slot());

    /* capability: the server mints a context for us and the reply carries it. */
    uint64_t answer[1] = {};
    bool cap_arrived = false;
    aegir::ipc::WordsReply const given = port.call_transfer(
        kMethodCapability, nullptr, 0, 0, answer, 1, &cap_arrived);
    if (given.error != 0) {
        fail("capability answered an error");
    }
    if (!cap_arrived) {
        fail("capability carried no context");
    }
    if (!aegir::ipc::take_received_cap(mint)) {
        fail("the context capability did not move");
    }
    aegir::signal::Context_capability const context(mint, 0);

    /* signal: the server's serving receive sees the bit. */
    aegir::signal::Transmitter::signal(context);
    uint64_t in[1] = {};
    aegir::ipc::WordsReply const saw = port.call_words(kMethodSignal, nullptr, 0, in, 1);
    if (saw.error != 0 || saw.count != 1 || in[0] != 1) {
        fail("the server did not see the signal");
    }

    /* held: nothing is ready, so the answer comes from the notification branch
     * -- the wait the primitive is for. */
    aegir::ipc::WordsReply const waited =
        port.call_words(kMethodHeld, nullptr, 0, in, 1);
    if (waited.error != 0 || waited.count != 1 || in[0] != 1) {
        fail("the held reply did not come back");
    }

    write("  signal-smoke: ok, a context signalled and a held reply came back\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
    return 0;
}

/* Whether argv[0] -- the name the spawner was told, which is the manifest's --
 * is exactly `name`. */
bool named(char const *argv0, char const *name, uint32_t length) noexcept
{
    if (argv0 == nullptr) {
        return false;
    }
    uint32_t i = 0;
    for (; i < length; ++i) {
        if (argv0[i] != name[i]) {
            return false;
        }
    }
    return argv0[i] == '\0';
}

}  // namespace

int main(int argc, char *argv[])
{
    char const *const name = argc > 0 ? argv[0] : nullptr;
    if (named(name, kPortName, kPortNameLength)) {
        return serve();
    }
    return run_client();
}
