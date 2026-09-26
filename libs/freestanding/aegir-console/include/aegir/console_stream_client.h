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

/** Open a stream in `mode` with `prompt` (cooked mode), carrying `doorbell`
 *  -- the client's own notification the handler signals when there is
 *  something to read -- as a capability (zero for none, a client that polls).
 *  False when refused. */
inline bool stream_open(aegir::ipc::Consumer const &port, uint64_t mode,
                        char const *prompt, uint32_t prompt_length,
                        seL4_CPtr doorbell = 0) noexcept
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
        port.call_transfer(kStreamMethodOpen, out, words, doorbell, in, 1, nullptr);
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

/** Read queued bytes into `out`, at most `capacity`. Answers the byte count, 0
 *  when nothing is queued. The capacity travels as the call's input word, so
 *  the handler drains no more than the caller can hold -- a one-byte key read
 *  must not swallow the characters queued behind it. Tier 1 is a poll
 *  (specs/terminal.md). */
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

/** Ask the terminal to run `line` with `cwd` and the NUL-separated `environment`
 *  (the shell's, sent so the command inherits it), redirecting the command's
 *  standard input and output to `std_in`/`std_out` (empty for the console
 *  stream; specs/shell.md). True when it started. */
inline bool stream_run(aegir::ipc::Consumer const &port, char const *line,
                       uint32_t line_length, char const *cwd, uint32_t cwd_length,
                       char const *environment, uint32_t environment_length,
                       char const *std_in, uint32_t std_in_length, char const *std_out,
                       uint32_t std_out_length) noexcept
{
    uint64_t out[aegir::ipc::kMaxWords];
    uint32_t words =
        aegir::nmspace::pack_string(out, line, line_length, aegir::nmspace::kPathMax);
    if (words == 0) {
        return false;
    }
    uint32_t const cwd_words =
        aegir::nmspace::pack_string(out + words, cwd, cwd_length, aegir::nmspace::kPathMax);
    if (cwd_words == 0) {
        return false;
    }
    words += cwd_words;
    uint32_t const environment_words = aegir::nmspace::pack_string(
        out + words, environment, environment_length, aegir::nmspace::kPathMax);
    if (environment_words == 0) {
        return false;
    }
    words += environment_words;
    uint32_t const in_words =
        aegir::nmspace::pack_string(out + words, std_in, std_in_length,
                                    aegir::nmspace::kPathMax);
    if (in_words == 0) {
        return false;
    }
    words += in_words;
    uint32_t const out_words2 =
        aegir::nmspace::pack_string(out + words, std_out, std_out_length,
                                    aegir::nmspace::kPathMax);
    if (out_words2 == 0) {
        return false;
    }
    words += out_words2;
    if (words > aegir::ipc::kMaxWords) {
        return false;
    }
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        port.call_words(kStreamMethodRun, out, words, in, 1);
    return answer.error == 0 && answer.count == 1 && in[0] == 1;
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

}  // namespace aegir::console

#endif  // AEGIR_CONSOLE_STREAM_CLIENT_H
