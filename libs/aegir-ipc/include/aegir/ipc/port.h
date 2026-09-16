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
 * protocol on top belongs to the port's owner (specs/services.md), and the
 * out-of-line buffer, when something needs to send a string, is the next
 * version of that format rather than something each port invents.
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

    /** Answer, then wait for the next call. One call needs one reply, and
     *  replying before receiving again is what keeps the caller's `call` a single
     *  round trip. */
    void reply(uint64_t word) noexcept;

private:
    seL4_CPtr capability_;
};

}  // namespace aegir::ipc

#endif  // AEGIR_IPC_PORT_H
