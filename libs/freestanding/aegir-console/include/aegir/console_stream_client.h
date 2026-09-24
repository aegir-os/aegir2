/*
 * The CON: stream's client call helpers (specs/terminal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The client half of aegir/console_stream.h: a program that opens a stream on
 * the terminal's port and reads and writes it. Kept apart from the wire
 * vocabulary so the handler -- which is host-tested -- need not see a kernel
 * header.
 */

#ifndef AEGIR_CONSOLE_STREAM_CLIENT_H
#define AEGIR_CONSOLE_STREAM_CLIENT_H

#include <aegir/console_stream.h>
#include <aegir/ipc/port.h>

namespace aegir::console {

/** Open a stream in `mode` with `prompt` (cooked mode). False when refused. */
inline bool stream_open(aegir::ipc::Consumer const &port, uint64_t mode,
                        char const *prompt, uint32_t prompt_length) noexcept
{
    uint64_t out[1 + aegir::nmspace::kPathMax / 8 + 1];
    out[0] = mode;
    uint32_t const words =
        1 + aegir::nmspace::pack_string(out + 1, prompt, prompt_length,
                                        aegir::nmspace::kPathMax);
    if (words == 1) {
        return false;
    }
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        port.call_words(kStreamMethodOpen, out, words, in, 1);
    return answer.error == 0 && answer.count == 1 && in[0] == 1;
}

/** Write `length` bytes. Answers how many were written. */
inline uint32_t stream_write(aegir::ipc::Consumer const &port, char const *bytes,
                             uint32_t length) noexcept
{
    uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
    uint32_t const words =
        aegir::nmspace::pack_string(out, bytes, length, aegir::nmspace::kPathMax);
    if (words == 0) {
        return 0;
    }
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        port.call_words(kStreamMethodWrite, out, words, in, 1);
    if (answer.error != 0 || answer.count != 1) {
        return 0;
    }
    return static_cast<uint32_t>(in[0]);
}

/** Read one line into `out`. Answers the byte count, 0 when none is ready. */
inline uint32_t stream_read_line(aegir::ipc::Consumer const &port, char *out,
                                 uint32_t capacity) noexcept
{
    uint64_t in[aegir::ipc::kMaxWords];
    aegir::ipc::WordsReply const answer =
        port.call_words(kStreamMethodReadLine, nullptr, 0, in, aegir::ipc::kMaxWords);
    if (answer.error != 0) {
        return 0;
    }
    char const *text = nullptr;
    uint32_t length = 0;
    if (!aegir::nmspace::unpack_string(in, answer.count, kStreamBytesMax, &text,
                                       &length) ||
        length > capacity) {
        return 0;
    }
    for (uint32_t i = 0; i < length; ++i) {
        out[i] = text[i];
    }
    return length;
}

/** Close the stream, reporting the status it finished with. False when the
 *  call itself was refused (closing a stream the handler did not have is
 *  still a close: the empty answer is the success). */
inline bool stream_close(aegir::ipc::Consumer const &port, uint64_t status) noexcept
{
    uint64_t out[1] = {status};
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        port.call_words(kStreamMethodClose, out, 1, in, 1);
    return answer.error == 0;
}

/** A command's end-of-run report: the status it finished with, without
 *  closing the stream it shares with its shell. False when refused. */
inline bool stream_exit(aegir::ipc::Consumer const &port, uint64_t status) noexcept
{
    uint64_t out[1] = {status};
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        port.call_words(kStreamMethodExit, out, 1, in, 1);
    return answer.error == 0;
}

}  // namespace aegir::console

#endif  // AEGIR_CONSOLE_STREAM_CLIENT_H
