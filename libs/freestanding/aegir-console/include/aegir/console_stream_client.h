/*
 * The CON: stream's client call helpers (specs/terminal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The client half of aegir/console_stream.h: a program that opens a stream on
 * the terminal's port and reads and writes it. Kept apart from the wire
 * vocabulary so the handler -- which is host-tested -- need not see a kernel
 * header. Starting a program is not here: that is the launcher's, and its
 * client is aegir/launch_client.h (specs/launch.md).
 */

#ifndef AEGIR_CONSOLE_STREAM_CLIENT_H
#define AEGIR_CONSOLE_STREAM_CLIENT_H

#include <aegir/console_stream.h>
#include <aegir/ipc/port.h>

namespace aegir::console {

/** Open a stream in `mode` with `prompt` (cooked mode). False when refused.
 *  The call carries no capability: a read waits by holding its reply, so
 *  there is no doorbell to hand over (specs/signal.md). */
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
        port.call_transfer(kStreamMethodOpen, out, words, 0, in, 1, nullptr);
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

/** Announce a line, its stage count, and -- when the shell has them -- the pid
 *  of each stage (specs/signal.md, specs/process.md). The shell owns the line,
 *  so the shell says how many stages it has; the terminal reports `pipeline
 *  exited` from it rather than from a spawn it did not perform. The count goes
 *  before the first command starts (the command bracket the terminal keeps),
 *  and the pids after the launch answer, so the terminal's Ctrl-C sets **C** on
 *  the foreground command. `pids` must name exactly `stages` of them. */
inline void stream_line(aegir::ipc::Consumer const &port, uint32_t stages,
                        uint64_t const *pids = nullptr) noexcept
{
    uint64_t out[aegir::ipc::kMaxWords];
    out[0] = stages;
    uint32_t words = 1;
    if (pids != nullptr) {
        for (uint32_t i = 0; i < stages && words < aegir::ipc::kMaxWords; ++i) {
            out[words++] = pids[i];
        }
    }
    (void)port.call_words(kStreamMethodLine, out, words, nullptr, 0);
}

/** Read bytes into `out`, at most `capacity`. The capacity travels as the
 *  call's input word, so the handler drains no more than the caller can hold --
 *  a one-byte key read must not swallow the characters queued behind it. A
 *  positive capacity *waits*: the terminal holds the caller's reply until a key
 *  arrives or the command ends, so this call blocks (specs/signal.md). An empty
 *  answer is end of input. A capacity of zero is the tier-1 poll, which answers
 *  at once. Answers the byte count. */
inline uint32_t stream_read(aegir::ipc::Consumer const &port, char *out,
                            uint32_t capacity) noexcept
{
    uint64_t out_words[1] = {capacity};
    uint64_t in[aegir::ipc::kMaxWords];
    aegir::ipc::WordsReply const answer =
        port.call_words(kStreamMethodRead, out_words, 1, in, aegir::ipc::kMaxWords);
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

/** A command's end-of-run report: the status it finished with and its own
 *  badge, without closing the stream it shares with its shell. The badge is
 *  what lets the terminal tell a foreground command's exit from a background
 *  `Run`'s (specs/shell.md). False when refused. */
inline bool stream_exit(aegir::ipc::Consumer const &port, uint64_t status,
                        uint64_t badge) noexcept
{
    uint64_t out[2] = {status, badge};
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        port.call_words(kStreamMethodExit, out, 2, in, 1);
    return answer.error == 0;
}

/** Redraw a cooked stream's prompt. False when refused. */
inline bool stream_set_prompt(aegir::ipc::Consumer const &port, char const *prompt,
                              uint32_t length) noexcept
{
    uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
    uint32_t const words =
        aegir::nmspace::pack_string(out, prompt, length, aegir::nmspace::kPathMax);
    if (words == 0) {
        return false;
    }
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        port.call_words(kStreamMethodSetPrompt, out, words, in, 1);
    return answer.error == 0;
}

/** A finished command's status. True and fills `status` when one has finished
 *  (and clears the finished state); false when none has. */
inline bool stream_command_status(aegir::ipc::Consumer const &port,
                                  uint64_t *status) noexcept
{
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        port.call_words(kStreamMethodCommandStatus, nullptr, 0, in, 1);
    if (answer.error != 0 || answer.count != 1) {
        return false;
    }
    *status = in[0];
    return true;
}

/** The text area's size, so a pager can size a page to the window: rows then
 *  columns. False when refused. */
inline bool stream_size(aegir::ipc::Consumer const &port, uint32_t *rows,
                        uint32_t *columns) noexcept
{
    uint64_t in[2];
    aegir::ipc::WordsReply const answer =
        port.call_words(kStreamMethodSize, nullptr, 0, in, 2);
    if (answer.error != 0 || answer.count != 2) {
        return false;
    }
    *rows = static_cast<uint32_t>(in[0]);
    *columns = static_cast<uint32_t>(in[1]);
    return true;
}

/** Present the read-only failure view of a boot session whose sequence failed
 *  (specs/boot.md). True when the terminal took it. The shell sends this once,
 *  after reporting the status to auth, and then stops reading. */
inline bool stream_boot_fail(aegir::ipc::Consumer const &port) noexcept
{
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        port.call_words(kStreamMethodBootFail, nullptr, 0, in, 1);
    return answer.error == 0 && answer.count == 1 && in[0] == 1;
}

}  // namespace aegir::console

#endif  // AEGIR_CONSOLE_STREAM_CLIENT_H
