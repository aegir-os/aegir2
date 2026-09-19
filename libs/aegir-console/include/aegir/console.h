/*
 * The console's port: windows, and the pixels behind them.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the same shape as aegir/framebuffer.h: the console
 * includes it to serve, a client includes it to call. The pixels are not
 * in the protocol -- they are the client's slice of the console's arena,
 * mapped by the client and composited by the console, which mapped the
 * whole arena when it carved it (specs/console.md). What rides in the
 * envelope is the bookkeeping: the slice's frames, the windows, and the
 * damage that says what changed.
 *
 * This port carries capabilities, and says so (specs/services.md): `frame`
 * answers with one frame cap of the caller's slice, one per reply, the
 * registry's window_frame shape.
 */

#ifndef AEGIR_CONSOLE_H
#define AEGIR_CONSOLE_H

#include <aegir/input.h>
#include <aegir/ipc/port.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::console {

/** The port's name, in the class.instance shape every port is named in. */
constexpr char const kPortName[] = "console.gui";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

/** Attach: in: how many bytes of arena the caller wants, rounded up to
 *  whole frames by the console. Answer: two words -- the slice's frame
 *  bits and frame count. The frames themselves come one per `frame` call.
 *  The slice is the caller's to lay out: a window's backing is an offset
 *  within it, and the offset is the caller's choice. */
constexpr uint32_t kMethodAttach = 1;

/** Frame: in: the frame's index within the caller's slice. Answer: one
 *  capability -- the frame, from the console's pristine set, so the caller
 *  may be its first mapping. An index past the count is the empty reply. */
constexpr uint32_t kMethodFrame = 2;

/** Create a window: in: x and y on the screen, width and height in pixels,
 *  the backing's offset within the caller's slice, and flags. The backing
 *  is width * height * 4 bytes, rows packed (B8G8R8X8, the framebuffer's
 *  format). Answer: one word -- the window's id, zero when refused (the
 *  backing would fall outside the slice, or the window outside the
 *  screen). New windows sit on top, unless the backdrop flag says
 *  otherwise. */
constexpr uint32_t kMethodCreateWindow = 3;

/** create_window's flags. Backdrop: the Amiga screen as a shape of window
 *  -- the window enters the z-order at the bottom and nothing raises it,
 *  so it shows only where no other window covers it (the bureau's). */
constexpr uint64_t kWindowBackdrop = 1ull << 0;

/** Damage: in: the window's id and a rectangle in window-local pixels (x,
 *  y, width, height). The rectangle's pixels, as they stand in the slice,
 *  are composited to the screen -- painter's algorithm, clipped against
 *  the windows above -- and the gpu flushes. The answer is empty. */
constexpr uint32_t kMethodDamage = 4;

/** Destroy: in: the window's id. The window leaves the z-order, and what
 *  was under it is repainted. The answer is empty. */
constexpr uint32_t kMethodDestroyWindow = 5;

/** Reap: system authority's call (auth's, on session reclaim): every
 * window the badge held is destroyed and its slice is free whole. In: the
 * badge. The answer is empty. Joins the reclaim order in specs/auth.md. */
constexpr uint32_t kMethodReap = 6;

/** Listen: collect the client's event channel -- one notification,
 *  answered as a capability (the frame shape). Console appends events to
 *  the ring and signals; the client waits on the notification and drains
 *  the ring. One channel per client, events tagged with their window: a
 *  thread waits on one notification, where per-window endpoints would
 *  have been one blocked receive too many. */
constexpr uint32_t kMethodListen = 7;

/* The event channel. The ring is the slice's last 4 KiB page: the console
 * mapped the whole slice when it carved it, so appending is writing memory
 * it already has, and the client maps the page with the rest. Word 0 is
 * the write index (console's), word 1 the read index (the client's), both
 * monotonic; entries are two words each -- the event as aegir-input packs
 * it (type, code, value), then the window's id. One producer, one
 * consumer, one core: volatile is the whole synchronisation story, and the
 * notification is the wakeup. A client that stops reading fills the ring,
 * and a full ring drops -- the console never blocks on a client. */
constexpr uint64_t kEventRingBytes = 1ull << seL4_PageBits;
constexpr uint64_t kEventRingEntries = (kEventRingBytes / 8 - 2) / 2;

/* The event vocabulary (specs/console.md's event channel). The envelope is
 * aegir-input's packed word. */
constexpr uint16_t kEventKey = 1;     /* code: the raw code; value below */
constexpr uint16_t kEventPointer = 2; /* code: 0 motion, else the button */
constexpr uint16_t kEventFocus = 3;   /* value: 1 in, 0 out */

/* A key event's value: bits 0..15 the translated character -- the keymap
 * is console's, US layout v1, and an unmapped code carries zero -- and
 * bit 16 set for a press, clear for a release. */
constexpr uint32_t kKeyPressed = 1u << 16;

/* A pointer event's value: x in bits 0..15, y in bits 16..31, window-local.
 * A button event's code carries kButtonRelease for the up. */
constexpr uint16_t kButtonRelease = 0x8000;

/** The ring's address within a mapped slice. */
inline volatile uint64_t *event_ring(uint8_t *slice, uint64_t slice_bytes) noexcept
{
    return reinterpret_cast<volatile uint64_t *>(slice + (slice_bytes - kEventRingBytes));
}

/** Listen, and take the notification into `slot`. False when refused. */
inline bool listen(aegir::ipc::Consumer const &gui, seL4_CPtr slot) noexcept
{
    bool cap_arrived = false;
    uint64_t in[1];
    aegir::ipc::WordsReply const answered =
        gui.call_transfer(kMethodListen, nullptr, 0, 0, in, 0, &cap_arrived);
    return answered.error == 0 && cap_arrived && aegir::ipc::take_received_cap(slot);
}

/** Take the oldest event off the ring. False when the ring is empty. */
inline bool ring_take(volatile uint64_t *ring, uint64_t *event, uint64_t *window) noexcept
{
    uint64_t const read = ring[1];
    if (read == ring[0]) {
        return false;
    }
    volatile uint64_t const *entry = ring + 2 + (read % kEventRingEntries) * 2;
    *event = entry[0];
    *window = entry[1];
    ring[1] = read + 1;
    return true;
}

/* The client walk, the registry.h shape: small inline callers over the
 * port's Consumer. */

/** Attach and answer the slice's shape. False when refused. */
inline bool attach(aegir::ipc::Consumer const &gui, uint64_t bytes,
                   uint64_t *frame_bits, uint64_t *frames) noexcept
{
    uint64_t in[2];
    aegir::ipc::WordsReply const shape = gui.call_words(kMethodAttach, &bytes, 1, in, 2);
    if (shape.error != 0 || shape.count != 2) {
        return false;
    }
    *frame_bits = in[0];
    *frames = in[1];
    return true;
}

/** One frame of the caller's slice: the capability lands in `slot`. */
inline bool frame(aegir::ipc::Consumer const &gui, uint64_t index,
                  seL4_CPtr slot) noexcept
{
    bool cap_arrived = false;
    uint64_t in[1];
    aegir::ipc::WordsReply const answered =
        gui.call_transfer(kMethodFrame, &index, 1, 0, in, 1, &cap_arrived);
    return answered.error == 0 && cap_arrived && aegir::ipc::take_received_cap(slot);
}

/** Create a window; its id, or zero when refused. */
inline uint64_t create_window(aegir::ipc::Consumer const &gui, uint64_t x, uint64_t y,
                              uint64_t width, uint64_t height,
                              uint64_t backing_offset, uint64_t flags = 0) noexcept
{
    uint64_t out[6] = {x, y, width, height, backing_offset, flags};
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        gui.call_words(kMethodCreateWindow, out, 6, in, 1);
    if (answer.error != 0 || answer.count != 1) {
        return 0;
    }
    return in[0];
}

/** Damage a rectangle of a window. False when the call was refused. */
inline bool damage(aegir::ipc::Consumer const &gui, uint64_t window, uint64_t x,
                   uint64_t y, uint64_t width, uint64_t height) noexcept
{
    uint64_t out[5] = {window, x, y, width, height};
    uint64_t in[1];
    aegir::ipc::WordsReply const answer = gui.call_words(kMethodDamage, out, 5, in, 1);
    return answer.error == 0;
}

/** Destroy a window. False when the call was refused. */
inline bool destroy_window(aegir::ipc::Consumer const &gui, uint64_t window) noexcept
{
    aegir::ipc::Reply const answer = gui.call(kMethodDestroyWindow, window);
    return answer.error == 0;
}

}  // namespace aegir::console

#endif  // AEGIR_CONSOLE_H
