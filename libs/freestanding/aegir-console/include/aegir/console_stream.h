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
 *  mode). The call may carry one capability: the client's own doorbell, a
 *  notification the handler signals when there is something to read -- input
 *  queued, a line ready, a command finished (the `listen` shape, the client's
 *  way to park instead of poll; specs/terminal.md). A client that polls passes
 *  none. A badge holds one stream: a second `open` is refused. Answer: one
 *  word, 1 opened and 0 refused -- the answer the spec's "nothing" did not
 *  leave room for, because a client has to be able to tell. */
constexpr uint32_t kStreamMethodOpen = 1;

/** Write bytes. In: the bytes as a string. They land at the stream's cursor,
 *  wrap and scroll, and honor `\r`, `\b`, `\n` and tab. Answer: one word, the
 *  count written -- less than asked is the refusal. */
constexpr uint32_t kStreamMethodWrite = 2;

/** Read bytes. In: the most bytes the caller can take, or no word for the
 *  envelope's bound. A positive bound *waits*: when nothing is queued and the
 *  command has not ended, the handler holds the caller's reply capability and
 *  answers it when a key arrives or the command does (specs/signal.md). A
 *  bound of zero is the tier-1 poll -- answer now, empty when nothing is
 *  queued -- for a caller that will not wait. Answer: the queued bytes as a
 *  string, or an empty answer, which for a read that waited is end of input. */
constexpr uint32_t kStreamMethodRead = 3;

/** Read one line. Answer: the finished line as a string when the line editor
 *  has one, an empty answer otherwise. This is the cooked call; the shell
 *  loops on it. A cooked read begins the line editor if it is idle. */
constexpr uint32_t kStreamMethodReadLine = 4;

/** Close the stream. In: the status the client finished with, which the shell
 *  reports (specs/shell.md's `return code`). The stream is dropped and the
 *  handler forgets the line it was holding. Answer: nothing. */
constexpr uint32_t kStreamMethodClose = 5;

/** A command's end-of-run report. In: the status it finished with, then the
 *  command's **own badge** (specs/memory.md's per-process id, which the
 *  bootstrap block carries and the command reads back). The badge is how the
 *  terminal tells a background command's exit from the foreground line's: a
 *  foreground command's status is the shell's `return code`, and a background
 *  command (specs/shell.md's `Run`) is reaped without one. The stream stays
 *  open -- it is the shell's, and a command inherits a copy -- so this is
 *  distinct from `close`. Answer: nothing. */
constexpr uint32_t kStreamMethodExit = 6;

/** Change the prompt a cooked stream draws. In: the prompt as a string. A
 *  shell that changed directory redraws its prompt through this, rather than
 *  reopening the stream. Answer: nothing. */
constexpr uint32_t kStreamMethodSetPrompt = 7;

/** A finished command's status. Answer: one word, the status, when a command
 *  has finished on the stream; an empty answer otherwise. Reading it is what
 *  clears the finished state, so the shell prints one `return code` line and
 *  then draws the next prompt. */
constexpr uint32_t kStreamMethodCommandStatus = 9;

/** The text area's size. Answer: two words, the rows then the columns, so a
 *  pager can size a page to the window rather than a constant. */
constexpr uint32_t kStreamMethodSize = 10;

/** The boot session's Startup-Sequence failed (specs/boot.md): present the
 *  read-only failure view. In: nothing. Answer: one word, 1 when the stream is
 *  a boot session's and the view is up, 0 otherwise. The shell sends it once,
 *  after it has reported the status to auth, and then stops reading; the
 *  terminal shows the window and takes no more input. */
constexpr uint32_t kStreamMethodBootFail = 11;

/** Announce a line and its stage count (specs/signal.md). In: the number of
 *  stages the line has. The shell owns the line, so the shell says how many;
 *  the terminal reports `pipeline exited` from it rather than from a spawn it
 *  may no longer perform. Answer: nothing. */
constexpr uint32_t kStreamMethodLine = 12;

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
