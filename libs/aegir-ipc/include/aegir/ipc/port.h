/*
 * Aegir's ports: the way one service asks another for something.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * specs/services.md defines the model and the rights that make it hold. What
 * this library provides is the two halves a service actually uses:
 *
 *   - `Consumer`, for a port someone else owns: call it, get an answer;
 *   - `Owner`, for a port you own: receive, reply.
 *
 * The two are separate types on purpose, and not for tidiness. A port has
 * exactly one reader -- receiving on a shared endpoint would let any reader take
 * a call meant for someone else, and a thief that replies is indistinguishable
 * from the owner it impersonated -- so a process holds at most one of these two
 * for a given port, and the rights it was given are what decide which. A caller
 * cannot accidentally receive, because it was never given Read.
 *
 * The wire format is the same for every port: a method number and words. A
 * protocol on top belongs to the port's owner (specs/services.md). The first
 * version carried one word in each direction, which is what the boot set's
 * protocols needed; the multi-word form below is this version's answer to a
 * protocol that needs to send a string, and the ceiling on it is the kernel's
 * own -- the message registers there are (seL4_MsgMaxLength,
 * kernel/libsel4/include/sel4/constants.h:55), minus the method's word --
 * not a number this library chose. The transfer forms carry one capability
 * beside the words (specs/vfs.md), and the rights that make that work are the
 * kernel's rules, not options: the invoked capability needs Grant for
 * anything to transfer at all (kernel/src/kernel/thread.c:212-218), and a
 * reply that carries a cap needs it on the owner's receiving half, which the
 * reply capability inherits (kernel/manual/parts/ipc.tex, "Calling and
 * Replying").
 */

#ifndef AEGIR_IPC_PORT_H
#define AEGIR_IPC_PORT_H

#include <stdint.h>

extern "C" {
/* libsel4's headers are C++-safe, but keeping the include here means callers do
 * not have to think about it. */
#include <sel4/sel4.h>
}

namespace aegir::ipc {

/** The method number a port's protocol uses for its first method. Protocols are
 *  versioned by refusing methods they do not know, so a caller can tell. */
constexpr uint32_t kMethodEvent = 1;

/** One word of payload, in each direction. Enough for the boot set's protocols;
 *  a protocol that needs more words is a change to this envelope, which is why
 *  it is here and not in a service. */
struct Reply {
    /** The kernel's error label: zero when the call was delivered. */
    uint64_t error;
    /** The word the owner replied with. */
    uint64_t word;
};

/** The payload ceiling in words, each way: the message registers the kernel
 *  has, minus the one the method travels in. A protocol's rows are smaller;
 *  this is the bound a caller is refused at, not a size to aim for. */
constexpr uint32_t kMaxWords = 119;

/** A multi-word answer: the error label, as with Reply -- non-zero means the
 *  kernel refused the call, or the count was past the ceiling and the kernel
 *  was never asked -- and how many words the owner actually sent back. */
struct WordsReply {
    uint64_t error;
    uint32_t count;
};

/** A port we may write to: we can call it, and that is all. */
class Consumer {
public:
    Consumer() noexcept;
    explicit Consumer(seL4_CPtr capability) noexcept;

    /** Find the port this process was given under `name`, through the bootstrap
     *  block (specs/services.md). The result may be invalid. */
    static Consumer find(char const *name, uint32_t length) noexcept;

    bool valid() const noexcept { return capability_ != 0; }

    /** Ask the owner something and wait for the answer. `reply.error` is non-zero
     *  when the kernel refused the call -- a missing right, or an owner that is
     *  not there -- which is a fact the caller has to be able to see rather than
     *  read as a nonsense answer. */
    Reply call(uint32_t method, uint64_t word) const noexcept;

    /** The multi-word form: `out`/`out_count` words ride after the method, and
     *  the answer's words land in `in` (up to `in_capacity`; the reply's count
     *  says how many the owner sent). A count past kMaxWords is refused before
     *  the kernel is asked. */
    WordsReply call_words(uint32_t method, uint64_t const *out, uint32_t out_count,
                          uint64_t *in, uint32_t in_capacity) const noexcept;

    /** The transfer form (specs/vfs.md): like call_words, plus one capability
     *  each way, riding outside the words. `cap` of zero sends none. A cap the
     *  owner replies with lands in the scratch receive slot
     *  (aegir::bootstrap::kSlotReceiveCap) and `cap_received`, when given, says
     *  whether one arrived -- the caller's to move out (take_received_cap)
     *  before any next transfer, because a second one onto an occupied slot
     *  fails. */
    WordsReply call_transfer(uint32_t method, uint64_t const *out, uint32_t out_count,
                             seL4_CPtr cap, uint64_t *in, uint32_t in_capacity,
                             bool *cap_received) const noexcept;

private:
    seL4_CPtr capability_;
};

/** A port we own: only we may receive on it. */
class Owner {
public:
    Owner() noexcept;
    explicit Owner(seL4_CPtr capability) noexcept;

    static Owner find(char const *name, uint32_t length) noexcept;

    bool valid() const noexcept { return capability_ != 0; }

    /** Wait for the next call. Returns the method, fills the caller's word, and
     *  — when `badge` is given — the badge the kernel reports for the caller,
     *  which is who is asking rather than who they say they are. */
    uint32_t receive(uint64_t *word, seL4_Word *badge) noexcept;

    /** The multi-word form of receive(): the payload lands in `words` (up to
     *  `capacity`), and `count` says how many words *arrived* -- more than
     *  `capacity` is a protocol break the owner can see, not something
     *  silently trimmed. When the call carried a capability it is in the
     *  scratch receive slot and `cap_arrived`, when given, says so -- the
     *  owner's to move out (take_received_cap) before the next receive. */
    uint32_t receive_words(uint64_t *words, uint32_t capacity, uint32_t *count,
                           seL4_Word *badge, bool *cap_arrived = nullptr) noexcept;

    /** Answer, then wait for the next call. One call needs one reply, and
     *  replying before receiving again is what keeps the caller's `call` a single
     *  round trip. */
    void reply(uint64_t word) noexcept;

    /** The multi-word form of reply(). A count past the ceiling is the owner's
     *  own protocol bug, and is trimmed to it -- there is no channel to report
     *  it on, and a partial answer beats none. */
    void reply_words(uint64_t const *words, uint32_t count) noexcept;

    /** The transfer form of reply(): words plus one capability. The owner's
     *  receiving half needs Grant, because the reply capability inherits its
     *  grant from it (kernel/manual/parts/ipc.tex, "Calling and Replying"). */
    void reply_cap(uint64_t const *words, uint32_t count, seL4_CPtr cap) noexcept;

private:
    seL4_CPtr capability_;
};

/** Move a capability out of the scratch receive slot into `target`, which
 *  must be empty. False when the kernel refuses. What a transfer leaves in
 *  the scratch slot is the receiver's to put somewhere before the next one
 *  (aegir::bootstrap::kSlotReceiveCap). */
bool take_received_cap(seL4_CPtr target) noexcept;

}  // namespace aegir::ipc

#endif  // AEGIR_IPC_PORT_H
