/*
 * The CON: handler's stream: a client's character line (specs/terminal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the same shape as aegir/console.h: the terminal (the
 * CON: handler) includes it to serve, a client includes it and
 * aegir/console_stream_client.h to call. A stream is the client's, keyed by
 * the badge the kernel reports on its calls, so a badge holds one console
 * (specs/terminal.md). The window and the pixels are never in the protocol;
 * what rides in the envelope is text.
 *
 * The wire vocabulary is here and deliberately depends on nothing but the
 * namespace's string shape: the handler is a value that is host-tested
 * (scripts/check_terminal.py), so it must not pull a kernel header. The
 * client's call helpers, which do need the ipc envelope, are their own
 * header: aegir/console_stream_client.h.
 *
 * Strings travel in the namespace protocol's shape (aegir/nmspace.h), as the
 * spec says: a length word, then the bytes. The envelope's ceiling
 * (aegir/ipc's kMaxWords) is the refusal bound on a write, and a client with
 * more than that to say says it in pieces.
 */

#ifndef AEGIR_CONSOLE_STREAM_H
#define AEGIR_CONSOLE_STREAM_H

#include <aegir/nmspace.h>
#include <stdint.h>

namespace aegir::console {

/** The port's name, in the class.instance shape every port is named in. The
 *  terminal creates and serves it; it is not a manifest port, because each
 *  session's handler is its own (specs/terminal.md). */
constexpr char const kStreamPortName[] = "con.stream";
constexpr uint32_t kStreamPortNameLength = sizeof(kStreamPortName) - 1;

/** Open a stream. In: the mode, then the prompt as a string (unused in raw
 *  mode). A badge holds one stream: a second `open` is refused. Answer: one
 *  word, 1 opened and 0 refused -- the answer the spec's "nothing" did not
 *  leave room for, because a client has to be able to tell. */
constexpr uint32_t kStreamMethodOpen = 1;

/** Write bytes. In: the bytes as a string. They land at the stream's cursor,
 *  wrap and scroll, and honor `\r`, `\b`, `\n` and tab. Answer: one word, the
 *  count written -- less than asked is the refusal. */
constexpr uint32_t kStreamMethodWrite = 2;

/** Read bytes. Answer: the queued bytes as a string, or an empty answer.
 *  Tier 1 is a poll; blocking read is a later method (specs/terminal.md). */
constexpr uint32_t kStreamMethodRead = 3;

/** Read one line. Answer: the finished line as a string when the line editor
 *  has one, an empty answer otherwise. This is the cooked call; the shell
 *  loops on it. A cooked read begins the line editor if it is idle. */
constexpr uint32_t kStreamMethodReadLine = 4;

/** Close the stream. In: the status the client finished with, which the shell
 *  reports (specs/shell.md's `return code`). The stream is dropped and the
 *  handler forgets the line it was holding. Answer: nothing. */
constexpr uint32_t kStreamMethodClose = 5;

/** A command's end-of-run report. In: the status it finished with. The stream
 *  stays open -- it is the shell's, and a command inherits a copy -- so this
 *  is distinct from `close`. It is the interim the spec records: a command
 *  talks through Aegir's own API until `exit()` carries the status itself
 *  (Phase 4), and this is where the shell's `return code` line gets its
 *  number. Answer: nothing. */
constexpr uint32_t kStreamMethodExit = 6;

/** A stream's discipline. Cooked owns the line editor; raw delivers keys as
 *  bytes, and a program that wants an editor's control reads raw and draws
 *  itself (specs/terminal.md). */
constexpr uint64_t kStreamModeRaw = 0;
constexpr uint64_t kStreamModeCooked = 1;

/** The most bytes one write or one line may carry: the envelope's ceiling,
 *  less the length word. The namespace's path bound is the same number, so
 *  the string shape and this bound agree by construction. */
constexpr uint32_t kStreamBytesMax = aegir::nmspace::kPathMax;

}  // namespace aegir::console

#endif  // AEGIR_CONSOLE_STREAM_H
