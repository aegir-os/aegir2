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

Reply Consumer::call(uint32_t method, uint64_t word) noexcept
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

void Owner::reply(uint64_t word) noexcept
{
    seL4_SetMR(kReplyMr, word);
    seL4_MessageInfo_t const info = seL4_MessageInfo_new(0, 0, 0, (seL4_Word)kReplyMr + 1);
    seL4_Reply(info);
}

}  // namespace aegir::ipc
