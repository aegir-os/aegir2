/*
 * aegir/signal.h: readiness without polling (specs/signal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One notification, bound to the serving thread, with a badge bit per waitable
 * source. A signal is a tagged wakeup, so a coalesced repeat loses nothing: the
 * bit says which source fired, not how many times. The receiver owns the
 * notification and hands out a minted, badged capability per context; the
 * transmitter signals that capability; a call whose answer genuinely waits for
 * the world holds its reply capability instead and answers when the world moves
 * (seL4_CNode_SaveCaller, kernel/manual/parts/ipc.tex "Calling and Replying").
 *
 * Two kernel facts shape this, and both are why it is one object rather than
 * several (specs/signal.md): a thread has one blocked receive, so a server
 * cannot wait on its port and a second object at once; and a thread-to-thread
 * message carries one capability, so a wakeup capability can never ride beside
 * a session capability (kernel/manual/parts/ipc.tex:160). The notification is
 * therefore bound to the serving thread, and calls and signals arrive on the
 * same receive.
 *
 * A caller's badge and a signal arrive together, and only the badge tells them
 * apart. Director mints a caller half with kCallMark set (aegir/ipc/port.h,
 * ports.cc:157), and a context's mint carries only its own bit, so is_signal is
 * the mark's test -- never "any high bit", because a user badge carries its
 * class at bit 62 (aegir/ipc/port.h).
 *
 * The library is policy-free: it does not decide who may signal whom. A service
 * chooses what a context means, and a context capability is revoked when the
 * session that minted it closes, because it was minted from that session's own
 * notification.
 */

#ifndef AEGIR_SIGNAL_H
#define AEGIR_SIGNAL_H

#include <aegir/ipc/port.h>
#include <aegir/mem/allocator.h>

#include <stdint.h>

namespace aegir::signal {

/** How many waitable sources one notification carries: the badge bits below
 *  the caller marks, which is the room a context has. */
constexpr unsigned kMaxContexts = 32;

/** One waitable source. It owns a badge bit of the receiver's notification, so
 *  a signal is a tagged wakeup and a coalesced repeat loses nothing -- the
 *  identity is the bit, not a count. */
class Context {
public:
    Context() noexcept = default;
    explicit Context(seL4_Word bit) noexcept : bit_(bit) {}

    bool valid() const noexcept { return bit_ != 0; }
    /** The badge this context's capability is minted with. */
    seL4_Word badge() const noexcept { return bit_; }

private:
    seL4_Word bit_ = 0;
};

/** A capability that signals a notification: what a transmitter holds. Badged
 *  with its context's bit, or with zero for a notification that carries no
 *  context -- a service that wakes one client through a notification of its
 *  own (the console's event channel, aegir/console.h). */
class Context_capability {
public:
    Context_capability() noexcept = default;
    Context_capability(seL4_CPtr cap, seL4_Word badge) noexcept : cap_(cap), badge_(badge) {}

    bool valid() const noexcept { return cap_ != 0; }
    seL4_CPtr cap() const noexcept { return cap_; }
    seL4_Word badge() const noexcept { return badge_; }

private:
    seL4_CPtr cap_ = 0;
    seL4_Word badge_ = 0;
};

/** The object a server waits on: one notification, bound to its serving thread,
 *  and the contexts it names. Its `notification()` is what the server's port
 *  receive waits with, so one receive sees calls and signals. */
class Receiver {
public:
    /** Make the notification and bind it to `tcb`, the thread that serves the
     *  port. `root` and `depth` address a slot in the caller's own CSpace: a
     *  service passes `bootstrap::kSlotOwnCNode` with `bootstrap::cnode_bits()`,
     *  the root task the same root with `seL4_WordBits` -- the caller knows its
     *  own layout and the kernel cannot tell it (aegir-spawn's `install` says
     *  why). False when the notification cannot be made or bound, and the
     *  caller then has no wakeup rather than a broken one. */
    bool init(aegir::mem::Allocator &objects, aegir::mem::Account &account, seL4_CPtr tcb,
              seL4_CPtr root, seL4_Word depth) noexcept
    {
        seL4_Error error = seL4_NoError;
        seL4_CPtr const notification = objects.alloc_object(
            seL4_NotificationObject, seL4_NotificationBits, account, &error);
        if (notification == 0) {
            return false;
        }
        if (!adopt(notification, tcb, root, depth)) {
            return false;
        }
        return true;
    }

    /** Adopt a notification the caller already made and bind it to `tcb`. A
     *  service with no delegated untyped makes its own the only way it can --
     *  retyped from a chunk `mem.main` handed it (specs/memory.md) -- and this
     *  is the half of `init` that takes it. The root and depth address the
     *  caller's slots, as `init`'s do. */
    bool adopt(seL4_CPtr notification, seL4_CPtr tcb, seL4_CPtr root,
               seL4_Word depth) noexcept
    {
        root_ = root;
        depth_ = depth;
        if (notification == 0 || tcb == 0 ||
            seL4_TCB_BindNotification(tcb, notification) != seL4_NoError) {
            return false;
        }
        notification_ = notification;
        return true;
    }

    bool valid() const noexcept { return notification_ != 0; }
    seL4_CPtr notification() const noexcept { return notification_; }

    /** A new context: the next free badge bit. Invalid when the word is spent. */
    Context create() noexcept
    {
        if (next_bit_ >= kMaxContexts) {
            return Context();
        }
        seL4_Word const bit = 1ull << next_bit_;
        ++next_bit_;
        mask_ |= bit;
        return Context(bit);
    }

    /** Mint `context`'s capability into `slot` (deleting whatever was there):
     *  Write only, because the signal is all a transmitter needs. The minted
     *  slot belongs to the caller, which moves or copies the capability to the
     *  transmitter and disposes of the slot as its own protocol says. Invalid
     *  when the notification or the context is not. */
    Context_capability mint(Context const &context, seL4_CPtr slot) noexcept
    {
        if (notification_ == 0 || !context.valid() || slot == 0) {
            return Context_capability();
        }
        seL4_CNode_Delete(root_, slot, depth_);
        if (seL4_CNode_Mint(root_, slot, depth_, root_, notification_, depth_,
                            seL4_CapRights_new(0, 0, 0, 1),
                            context.badge()) != seL4_NoError) {
            return Context_capability();
        }
        return Context_capability(slot, context.badge());
    }

    /** Whether a badge the serving receive returned is a signal rather than a
     *  call: one of our bits, and none of anyone else's. */
    bool is_signal(seL4_Word badge) const noexcept
    {
        return badge != 0 && (badge & aegir::ipc::kCallMark) == 0 && (badge & ~mask_) == 0;
    }

    /** Whether `context` fired in this signal. */
    bool ready(seL4_Word badge, Context const &context) const noexcept
    {
        return context.valid() && (badge & context.badge()) != 0;
    }

private:
    seL4_CPtr notification_ = 0;
    seL4_CPtr root_ = 0;
    seL4_Word depth_ = 0;
    seL4_Word mask_ = 0;
    unsigned next_bit_ = 0;
};

/** The other side: signal a context a service handed out. */
class Transmitter {
public:
    static void signal(Context_capability const &capability) noexcept
    {
        if (capability.valid()) {
            seL4_Signal(capability.cap());
        }
    }
};

/** A held reply: the caller's reply capability is saved now and answered later,
 *  optionally carrying a capability (kernel/manual/parts/ipc.tex, "Calling and
 *  Replying"). Used when the answer is "later", not "not yet" -- the caller
 *  waits inside its call and the server stays free. `root`/`depth`/`slot`
 *  address the slot the saved reply lands in, the caller's own, as `Receiver`
 *  explains. */
class Reply_holder {
public:
    Reply_holder() noexcept = default;
    Reply_holder(seL4_CPtr root, seL4_Word depth, seL4_CPtr slot) noexcept
        : root_(root), depth_(depth), slot_(slot) {}

    bool valid() const noexcept { return slot_ != 0; }
    seL4_CPtr slot() const noexcept { return slot_; }
    bool held() const noexcept { return held_; }

    /** Save the caller's reply capability. False when there was no caller to
     *  hold -- a malformed call, which the service should refuse rather than
     *  leave the caller waiting forever. */
    bool save() noexcept
    {
        if (slot_ == 0 ||
            seL4_CNode_SaveCaller(root_, slot_, depth_) != seL4_NoError) {
            return false;
        }
        held_ = true;
        return true;
    }

    /** Answer the held caller: `count` words of its own envelope, optionally
     *  carrying one capability. The holder is empty afterwards, whatever the
     *  reply did, because the reply capability is spent. */
    void reply(uint64_t const *words, uint32_t count, seL4_CPtr cap) noexcept
    {
        if (!held_) {
            return;
        }
        uint32_t const sent = count < aegir::ipc::kMaxWords ? count : aegir::ipc::kMaxWords;
        for (uint32_t i = 0; i < sent; ++i) {
            seL4_SetMR(i, words[i]);
        }
        seL4_Word const extra = cap != 0 ? 1 : 0;
        if (extra != 0) {
            seL4_SetCap(0, cap);
        }
        seL4_Send(slot_, seL4_MessageInfo_new(0, 0, extra, sent));
        held_ = false;
    }

    /** The one-word form, which is what a driver's `next` answers with. */
    void reply(uint64_t word) noexcept { reply(&word, 1, 0); }

private:
    seL4_CPtr root_ = 0;
    seL4_Word depth_ = 0;
    seL4_CPtr slot_ = 0;
    bool held_ = false;
};

}  // namespace aegir::signal

#endif  // AEGIR_SIGNAL_H
