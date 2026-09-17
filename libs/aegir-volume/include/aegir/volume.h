/*
 * The protocol a filesystem serves on its volume port, and nothing else.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A volume is what a filesystem registers with the VFS (specs/vfs.md); this
 * is the wire its clients then speak, on the capability resolve handed
 * them. Version one is stateless -- no handles, no per-client state: the
 * shape a system with many concurrent readers wants, and read-only
 * filesystems have nothing to synchronize.
 *
 *   - `read`: a path (everything after the volume's colon), an offset, and
 *     how many bytes the caller will take. The answer is a byte count, an
 *     end-of-file flag, and the bytes, packed. A filesystem that serves
 *     from a shared window copies out inside the one call: the window's
 *     "content belongs to the most recent call" caveat never reaches the
 *     volume's clients.
 *   - `list`: a path and an index. The answer is one entry -- name, size,
 *     kind -- or nothing at the end of the directory. The cursor is the
 *     caller's index; the directory owes no stability across calls.
 *
 * Paths after the colon are the filesystem's to interpret, including the
 * Amiga `/`-is-parent convention and case sensitivity (specs/vfs.md). The
 * string wire shape is the namespace protocol's (aegir/nmspace.h): one
 * shape for every string a port carries.
 */

#pragma once

#include <stdint.h>

namespace aegir::volume {

constexpr uint32_t kMethodRead = 1; /* in: path words, offset, max; answer: count, eof, bytes */
constexpr uint32_t kMethodList = 2; /* in: path words, index; answer: name words, size, kind */

/** Entry kinds a list answer reports. */
constexpr uint64_t kKindFile = 1;
constexpr uint64_t kKindDir = 2;

/** The most data one read answer carries: the envelope's words, less the
 *  count and the flag, in bytes. A caller that wants more asks again at the
 *  next offset; a caller that wants it in one ask is the recorded scaling
 *  path (a buffer capability at open, specs/vfs.md). */
constexpr uint32_t kReadMax = 117 * 8;

/** read's answer: how many bytes follow, and whether the file ends there. */
constexpr uint32_t kReadHeaderWords = 2;

/** list's answer after the name string: the entry's size and its kind. */
constexpr uint32_t kListTailWords = 2;

}  // namespace aegir::volume
