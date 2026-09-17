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
 * a plain slot number at kCNodeBits (aegir/bootstrap.h). Setting the path is
 * an IPC-buffer store, not a syscall, so it is done on every call and
 * receive rather than kept as state to get stale. */
void set_receive_path() noexcept
{
    seL4_SetCapReceivePath(bootstrap::kSlotOwnCNode, bootstrap::kSlotReceiveCap,
                           bootstrap::kCNodeBits);
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
    return length > kMethodMr ? seL4_GetMR(kMethodMr) : 0;
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
    return seL4_CNode_Move(bootstrap::kSlotOwnCNode, target, bootstrap::kCNodeBits,
                           bootstrap::kSlotOwnCNode, bootstrap::kSlotReceiveCap,
                           bootstrap::kCNodeBits) == seL4_NoError;
}

}  // namespace aegir::ipc
