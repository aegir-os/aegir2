/*
 * The input port's protocol: what an input driver serves.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the same shape as aegir/block.h and aegir/entropy.h:
 * the driver includes it to serve, a client includes it to call. An event
 * is virtio's own structure (virtio 1.x, 5.8.6.1) -- a type, a code, and a
 * value, Linux's input codes carried unchanged -- and it rides in the
 * port's envelope as one word, so there is no shared window.
 */

#ifndef AEGIR_INPUT_H
#define AEGIR_INPUT_H

#include <stdint.h>

namespace aegir::input {

/** Poll: the answer's word is 1 when an event waits, 0 when none does. */
constexpr uint32_t kMethodPoll = 1;

/** Next: the answer's word is the next event. When none has arrived the
 *  reply is *held* -- the caller's reply capability is saved and the answer
 *  crosses when the interrupt lands (specs/services.md). One waiter at a
 *  time: a second `next` while one is held answers empty, which is the
 *  protocol's "try again", not an event. */
constexpr uint32_t kMethodNext = 2;

/* The event as one word: type and code are 16 bits each, value 32. */
constexpr uint64_t pack_event(uint16_t type, uint16_t code, uint32_t value) noexcept
{
    return type | (static_cast<uint64_t>(code) << 16) | (static_cast<uint64_t>(value) << 32);
}

constexpr uint16_t event_type(uint64_t word) noexcept
{
    return static_cast<uint16_t>(word & 0xffff);
}

constexpr uint16_t event_code(uint64_t word) noexcept
{
    return static_cast<uint16_t>((word >> 16) & 0xffff);
}

constexpr uint32_t event_value(uint64_t word) noexcept
{
    return static_cast<uint32_t>(word >> 32);
}

/* The types a reader switches on (Linux's input-event codes, which
 * virtio-input carries unchanged): a key press or release is EV_KEY with
 * the key's code and 1 or 0 as the value; pointer motion is EV_REL (a
 * delta -- the mouse) or EV_ABS (a position -- the tablet); a pointer's
 * button is EV_KEY with a BTN_* code; EV_SYN ends one moment's worth of
 * events. */
constexpr uint16_t kEvSyn = 0;
constexpr uint16_t kEvKey = 1;
constexpr uint16_t kEvRel = 2;
constexpr uint16_t kEvAbs = 3;

/* The codes within them. X and Y are the same numbers under EV_REL and
 * EV_ABS; the buttons are EV_KEY codes. */
constexpr uint16_t kAxisX = 0;
constexpr uint16_t kAxisY = 1;
constexpr uint16_t kRelWheel = 8;
constexpr uint16_t kBtnLeft = 0x110;
constexpr uint16_t kBtnRight = 0x111;
constexpr uint16_t kBtnMiddle = 0x112;

}  // namespace aegir::input

#endif  // AEGIR_INPUT_H
