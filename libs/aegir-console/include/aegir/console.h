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
 *  and the backing's offset within the caller's slice. The backing is
 *  width * height * 4 bytes, rows packed (B8G8R8X8, the framebuffer's
 *  format). Answer: one word -- the window's id, zero when refused (the
 *  backing would fall outside the slice, or the window outside the
 *  screen). New windows sit on top. */
constexpr uint32_t kMethodCreateWindow = 3;

/** Damage: in: the window's id and a rectangle in window-local pixels (x,
 *  y, width, height). The rectangle's pixels, as they stand in the slice,
 *  are composited to the screen -- painter's algorithm, clipped against
 *  the windows above -- and the gpu flushes. The answer is empty. */
constexpr uint32_t kMethodDamage = 4;

/** Destroy: in: the window's id. The window leaves the z-order, and what
 *  was under it is repainted. The answer is empty. */
constexpr uint32_t kMethodDestroyWindow = 5;

/** Reap: system authority's call (auth's, on session reclaim): every
 *  window the badge held is destroyed and its slice is free whole. In: the
 *  badge. The answer is empty. Joins the reclaim order in specs/auth.md. */
constexpr uint32_t kMethodReap = 6;

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
                              uint64_t backing_offset) noexcept
{
    uint64_t out[5] = {x, y, width, height, backing_offset};
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        gui.call_words(kMethodCreateWindow, out, 5, in, 1);
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
