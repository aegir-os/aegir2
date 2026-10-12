/*
 * Aegir's ports -- implementation. See include/aegir/ipc/port.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/ipc/port.h>

#include <aegir/bootstrap.h>

namespace aegir::ipc {

namespace {
/* The method travels in the first word, the payload in the second. */
constexpr seL4_Word kMethodMr = 0;
constexpr seL4_Word kWordMr = 1;
constexpr seL4_Word kReplyMr = 0;

/* Where a transferred capability lands: one scratch slot, addressed the way
 * this process addresses its own CSpace everywhere -- the own-CNode cap with
 * a plain slot number at `bootstrap::endpoint_depth()` (aegir/bootstrap.h).
 * Setting the path is an IPC-buffer store, not a syscall, so it is done on
 * every call and receive rather than kept as state to get stale. */
/* The slot a transferred capability lands in is this layer's scratch
 * (bootstrap.h's kSlotReceiveCap), and it has to be *empty*: the kernel
 * delivers a capability only into an empty slot and drops it silently when it
 * is not, which reads a layer up as "the callee sent no capability at all"
 * (kernel/src/object/cnode.c:895-928). So the layer empties it before every
 * call and every receive: whatever is there belongs to a caller that did not
 * take it, and it is the *next* transfer that pays for it.
 *
 * Measured, which is what this is for: ld.lld's chunks from mem.main arrived as
 * answers with no capability (`heap:   error 0, asked 21 bits, words 2, cap
 * dropped`) while the service was carving and replying, and the link died of
 * `std::bad_alloc` a layer up. The caller that did not take its reply's
 * capability is a separate question; this is the layer refusing to be poisoned
 * by it. */
void set_receive_path() noexcept
{
    seL4_CNode_Delete(bootstrap::kSlotOwnCNode, bootstrap::kSlotReceiveCap,
                      bootstrap::endpoint_depth());
    seL4_SetCapReceivePath(bootstrap::kSlotOwnCNode, bootstrap::kSlotReceiveCap,
                           bootstrap::endpoint_depth());
}
}  // namespace

Consumer::Consumer() noexcept : capability_(0) {}

Consumer::Consumer(seL4_CPtr capability) noexcept : capability_(capability) {}

Consumer Consumer::find(char const *name, uint32_t length) noexcept
{
    uint64_t slot = 0;
    if (!bootstrap::capability(name, length, &slot)) {
        return Consumer();
    }
    return Consumer(slot);
}

Reply Consumer::call(uint32_t method, uint64_t word) const noexcept
{
    seL4_SetMR(kMethodMr, method);
    seL4_SetMR(kWordMr, word);
    seL4_MessageInfo_t const info = seL4_MessageInfo_new(0, 0, 0, kWordMr + 1);
    seL4_MessageInfo_t const answer = seL4_Call(capability_, info);
    Reply reply{};
    /* A refused invocation comes back as an error label instead of an answer
     * (libsel4's convention), so it is reported rather than read as data. */
    reply.error = seL4_MessageInfo_get_label(answer);
    reply.word = seL4_GetMR(kReplyMr);
    return reply;
}

WordsReply Consumer::call_words(uint32_t method, uint64_t const *out, uint32_t out_count,
                                uint64_t *in, uint32_t in_capacity) const noexcept
{
    WordsReply reply{seL4_InvalidArgument, 0};
    if (out_count > kMaxWords || in_capacity > kMaxWords) {
        return reply;
    }
    seL4_SetMR(kMethodMr, method);
    for (uint32_t i = 0; i < out_count; ++i) {
        seL4_SetMR(kWordMr + i, out[i]);
    }
    seL4_MessageInfo_t const info = seL4_MessageInfo_new(0, 0, 0, kWordMr + out_count);
    seL4_MessageInfo_t const answer = seL4_Call(capability_, info);
    reply.error = seL4_MessageInfo_get_label(answer);
    uint32_t const arrived =
        static_cast<uint32_t>(seL4_MessageInfo_get_length(answer));
    reply.count = arrived < in_capacity ? arrived : in_capacity;
    for (uint32_t i = 0; i < reply.count; ++i) {
        in[i] = seL4_GetMR(kReplyMr + i);
    }
    return reply;
}

WordsReply Consumer::call_transfer(uint32_t method, uint64_t const *out,
                                   uint32_t out_count, seL4_CPtr cap, uint64_t *in,
                                   uint32_t in_capacity, bool *cap_received) const noexcept
{
    WordsReply reply{seL4_InvalidArgument, 0};
    if (out_count > kMaxWords || in_capacity > kMaxWords) {
        return reply;
    }
    /* The caller's half of the wait pair, printed *before* the payload window
     * opens. The window is this function's first `seL4_SetMR(kMethodMr, ...)`
     * through its `seL4_Call`, and a print inside it -- which is what the first
     * version of this did, right after `set_receive_path()` -- sends the log
     * message instead of the caller's: 10 583 `vfs: ...: no such volume` lines in
     * a 29-second run, from a probe. So: before the first `SetMR`, never after it.
     *
     * Guarded, because the print is itself a port call that comes back through
     * here: a console call made by this line would print again, and that print
     * again. `probing` is the allocator's `growing_` shape -- the caller of a print
     * is the one that prints. The window key is the (caller, method) pair, so an
     * idling client is a couple of lines and a new request is news; the last
     * `ipc: call` with no `ipc: recv` behind it is the request that never came
     * back, and its badge is the process that stopped. */
    constexpr uint32_t kWindow = 8;
    static uint64_t seen_from[kWindow] = {};
    static uint32_t seen_method[kWindow] = {};
    static uint32_t seen_next = 0;
    static bool probing = false;
    if (!probing) {
        uint64_t who = 0;
        static_cast<void>(bootstrap::badge(&who));
        bool known = false;
        for (uint32_t i = 0; i < kWindow; ++i) {
            if (seen_from[i] == who && seen_method[i] == method) {
                known = true;
                break;
            }
        }
        if (!known) {
            seen_from[seen_next] = who;
            seen_method[seen_next] = method == 0 ? 1 : method;
            seen_next = (seen_next + 1) % kWindow;
            probing = true;
            aegir::debug_write("      ipc: call ");
            aegir::debug_write_unsigned(method);
            aegir::debug_write(" from ");
            aegir::debug_write_unsigned(who);
            aegir::debug_write("\n");
            probing = false;
        }
    }
    seL4_SetMR(kMethodMr, method);
    for (uint32_t i = 0; i < out_count; ++i) {
        seL4_SetMR(kWordMr + i, out[i]);
    }
    seL4_Word const extra = cap != 0 ? 1 : 0;
    if (extra != 0) {
        seL4_SetCap(0, cap);
    }
    set_receive_path();
    seL4_MessageInfo_t const info =
        seL4_MessageInfo_new(0, 0, extra, kWordMr + out_count);
    seL4_MessageInfo_t const answer = seL4_Call(capability_, info);
    reply.error = seL4_MessageInfo_get_label(answer);
    uint32_t const arrived =
        static_cast<uint32_t>(seL4_MessageInfo_get_length(answer));
    reply.count = arrived < in_capacity ? arrived : in_capacity;
    for (uint32_t i = 0; i < reply.count; ++i) {
        in[i] = seL4_GetMR(kReplyMr + i);
    }
    if (cap_received != nullptr) {
        /* A call that expected a cap back and got none -- extraCaps zero --
         * is how a missing Grant right shows up (kernel/src/kernel/
         * thread.c:212-218): the words arrive and the cap silently does not,
         * so the count is reported rather than assumed. */
        *cap_received = seL4_MessageInfo_get_extraCaps(answer) != 0;
    }
    return reply;
}

void Consumer::send_words(uint32_t method, uint64_t const *out, uint32_t out_count) const noexcept
{
    if (out_count > kMaxWords) {
        return;
    }
    seL4_SetMR(kMethodMr, method);
    for (uint32_t i = 0; i < out_count; ++i) {
        seL4_SetMR(kWordMr + i, out[i]);
    }
    /* The same envelope a call carries (the method's word, then the payload),
     * so the owner's receive_words reads it identically; the difference is the
     * invocation -- Send, not Call -- so no reply capability is left for the
     * owner to answer. */
    seL4_MessageInfo_t const info = seL4_MessageInfo_new(0, 0, 0, kWordMr + out_count);
    seL4_Send(capability_, info);
}

Owner::Owner() noexcept : capability_(0) {}

Owner::Owner(seL4_CPtr capability) noexcept : capability_(capability) {}

Owner Owner::find(char const *name, uint32_t length) noexcept
{
    uint64_t slot = 0;
    if (!bootstrap::capability(name, length, &slot)) {
        return Owner();
    }
    return Owner(slot);
}

uint32_t Owner::receive(uint64_t *word, seL4_Word *badge) noexcept
{
    seL4_Word sender = 0;
    /* A wait *and* its return, because one without the other is the difference
     * between a service doing its job and a service that has stopped. Measured
     * both ways: a bare entry print made the logger -- which waits between every
     * record -- the busiest line in the log and told us nothing about who was
     * stuck; and the run ends with the console frozen and host CPU at zero, which
     * a *returning* waiter never causes. So the last line of a dead run has to be
     * an unmatched `wait`, and the culprit is whoever owns it. Only the first
     * sight of each (event, owner, method) triple prints, so an idling service is
     * one line and a new waiter is news -- the same window as the caller probe. */
    constexpr uint32_t kWindow = 8;
    static uint64_t seen_owner[kWindow] = {};
    static uint32_t seen_next = 0;
    uint64_t owner = 0;
    static_cast<void>(bootstrap::badge(&owner));
    bool known = false;
    for (uint32_t i = 0; i < kWindow; ++i) {
        if (seen_owner[i] == owner) {
            known = true;
            break;
        }
    }
    if (!known) {
        seen_owner[seen_next] = owner == 0 ? 1 : owner;
        seen_next = (seen_next + 1) % kWindow;
        aegir::debug_write("      ipc: wait at ");
        aegir::debug_write_unsigned(owner);
        aegir::debug_write("\n");
    }
    seL4_MessageInfo_t const info = seL4_Recv(capability_, &sender);
    if (badge != nullptr) {
        *badge = sender;
    }
    if (word != nullptr) {
        *word = seL4_GetMR(kWordMr);
    }
    return seL4_MessageInfo_get_length(info) > kMethodMr ? seL4_GetMR(kMethodMr) : 0;
}

uint32_t Owner::receive_words(uint64_t *words, uint32_t capacity, uint32_t *count,
                              seL4_Word *badge, bool *cap_arrived) noexcept
{
    seL4_Word sender = 0;
    set_receive_path();
    seL4_MessageInfo_t const info = seL4_Recv(capability_, &sender);
    if (badge != nullptr) {
        *badge = sender;
    }
    if (cap_arrived != nullptr) {
        *cap_arrived = seL4_MessageInfo_get_extraCaps(info) != 0;
    }
    uint32_t const length = static_cast<uint32_t>(seL4_MessageInfo_get_length(info));
    uint32_t const arrived = length > kWordMr ? length - kWordMr : 0;
    uint32_t const taken = arrived < capacity ? arrived : capacity;
    for (uint32_t i = 0; i < taken; ++i) {
        words[i] = seL4_GetMR(kWordMr + i);
    }
    if (count != nullptr) {
        *count = arrived;
    }
    uint32_t const method = length > kMethodMr ? seL4_GetMR(kMethodMr) : 0;
    /* Which caller-and-method pairs this service has already said out loud -- a
     * *window*, not one remembered pair and not the whole history. Both of those
     * failed measurably: a print per call buried the console (2.6 M lines, and
     * the guest stopped reaching the link), and a single remembered pair hid the
     * linker entirely -- all 31 of its asks to `mem.main` were consecutive, so
     * only the first was "a change". The window's size decides only how much
     * *repetition* is suppressed: the first sight of any pair always prints, so a
     * caller that repeats after other traffic re-enters it and is seen. Only a
     * user badge's calls, because a service's own chatter is not what a stuck
     * acceptance is about. */
    constexpr uint32_t kWindow = 8;
    static uint64_t seen_from[kWindow] = {};
    static uint32_t seen_method[kWindow] = {};
    static uint32_t seen_next = 0;
    if (aegir::ipc::is_user_badge(sender)) {
        bool known = false;
        for (uint32_t i = 0; i < kWindow; ++i) {
            if (seen_from[i] == sender && seen_method[i] == method) {
                known = true;
                break;
            }
        }
        if (!known) {
            seen_from[seen_next] = sender;
            seen_method[seen_next] = method;
            seen_next = (seen_next + 1) % kWindow;
            uint64_t owner = 0;
            static_cast<void>(bootstrap::badge(&owner));
            aegir::debug_write("      ipc: recv method ");
            aegir::debug_write_unsigned(method);
            aegir::debug_write(" at ");
            aegir::debug_write_unsigned(owner);
            aegir::debug_write(" from ");
            aegir::debug_write_unsigned(sender);
            aegir::debug_write("\n");
        }
    }
    return method;
}

void Owner::reply(uint64_t word) noexcept
{
    seL4_SetMR(kReplyMr, word);
    seL4_MessageInfo_t const info = seL4_MessageInfo_new(0, 0, 0, (seL4_Word)kReplyMr + 1);
    seL4_Reply(info);
}

void Owner::reply_words(uint64_t const *words, uint32_t count) noexcept
{
    uint32_t const sent = count < kMaxWords ? count : kMaxWords;
    for (uint32_t i = 0; i < sent; ++i) {
        seL4_SetMR(kReplyMr + i, words[i]);
    }
    seL4_MessageInfo_t const info = seL4_MessageInfo_new(0, 0, 0, (seL4_Word)sent);
    seL4_Reply(info);
}

void Owner::reply_cap(uint64_t const *words, uint32_t count, seL4_CPtr cap) noexcept
{
    uint32_t const sent = count < kMaxWords ? count : kMaxWords;
    for (uint32_t i = 0; i < sent; ++i) {
        seL4_SetMR(kReplyMr + i, words[i]);
    }
    seL4_SetCap(0, cap);
    seL4_MessageInfo_t const info = seL4_MessageInfo_new(0, 0, 1, (seL4_Word)sent);
    seL4_Reply(info);
}

bool take_received_cap(seL4_CPtr target) noexcept
{
    return seL4_CNode_Move(bootstrap::kSlotOwnCNode, target, bootstrap::endpoint_depth(),
                           bootstrap::kSlotOwnCNode, bootstrap::kSlotReceiveCap,
                           bootstrap::endpoint_depth()) == seL4_NoError;
}

void drop_received_cap() noexcept
{
    seL4_CNode_Delete(bootstrap::kSlotOwnCNode, bootstrap::kSlotReceiveCap,
                      bootstrap::endpoint_depth());
}

}  // namespace aegir::ipc
