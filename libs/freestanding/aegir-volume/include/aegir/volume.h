/*
 * The protocol a filesystem serves on its volume port, and nothing else.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A volume is what a filesystem registers with the VFS (specs/vfs.md); this
 * is the wire its clients then speak, on the capability resolve handed
 * them. Reads and listings are stateless -- no handles, no per-client
 * state: the shape a system with many concurrent readers wants. Writes are
 * handles, because a usable userspace API is one: open with its mode flags,
 * write at the cursor, close. The string wire shape is the namespace
 * protocol's (aegir/nmspace.h): one shape for every string a port carries.
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
 *   - `open`: a path and mode flags (create, truncate). The answer is one
 *     word, the handle; zero is the refusal -- a bad path, an existing
 *     name without create, a read-only volume. Truncate frees the file's
 *     old chain the moment the open answers.
 *   - `write`: a handle and bytes, packed after the count. The bytes land
 *     at the cursor, the cursor advances, and the file extends when the
 *     cursor crosses its end. The answer is the count written; less than
 *     asked is the refusal.
 *   - `close`: a handle. The answer is 1, or 0 when the handle was not
 *     one; the row is freed and the serial is never reused.
 *
 * A handle is scoped to the caller's badge: resolve minted the client's
 * copy of the volume port with it, so a handle named by any other badge is
 * not one.
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

/* The write side (specs/vfs.md): handles, the only per-client state a
 * filesystem holds. A handle answers open, is scoped to the caller's badge
 * -- a handle named by any other badge is not one -- and its serial is never
 * reused. Zero is never a handle: it is open's refusal. */
constexpr uint32_t kMethodOpen = 3;  /* in: path words, mode flags; answer: handle */
constexpr uint32_t kMethodWrite = 4; /* in: handle, count, bytes; answer: written */
constexpr uint32_t kMethodClose = 5; /* in: handle; answer: 1, or 0 */
/* mkdir: a path. The directory it names is made, and every missing
 * component on the way -- the mmd shape, because "ensure the home exists"
 * is one call, not a walk (specs/auth.md). Existing components are fine.
 * Answer: 1, or 0 -- a component that is a file, an invalid name, a
 * read-only or full volume. */
constexpr uint32_t kMethodMkdir = 6; /* in: path words; answer: 1, or 0 */
/* remove: a path. A file's chain is freed and its slot marked deleted; a
 * directory only when it holds nothing but dot and dotdot -- a tree dies
 * leaf-first. FAT has no link counts, so a file an open handle names is
 * refused rather than unlinked under the writer. Answer: 1, or 0 -- not
 * found, not empty, open, read-only, or the root. */
constexpr uint32_t kMethodRemove = 7; /* in: path words; answer: 1, or 0 */
/* reap: a badge. Every handle the badge holds is dropped, as though
 * closed -- the session teardown's mechanism, called today by whoever
 * knows the badge (specs/vfs.md). Answer: how many were dropped. */
constexpr uint32_t kMethodReap = 8;   /* in: badge; answer: how many */
/* stat: a path. The walk ends at the thing the path names -- the empty path
 * is the root, always a directory. Answer: the kind and the size
 * (kStatTailWords words), or nothing when the path is not there. A directory
 * has no size, so its size word is zero. Like list, stat needs no handle and
 * no per-client state; it is what `std::filesystem::status` stands on
 * (specs/cxx.md step 5). */
constexpr uint32_t kMethodStat = 9;   /* in: path words; answer: kind, size */

/* rename: a source path and a destination path. The source entry (its
 * long-name run and its 8.3 slot) is deleted and the destination entry is
 * made in its place, carrying the same first cluster and size -- the data
 * does not move. Both names must live in the same directory; a destination
 * that already exists, an open handle on the source, and a read-only volume
 * are refused. Answer: 1, or 0 (specs/vfs.md). */
constexpr uint32_t kMethodRename = 10; /* in: src path words, dst path words; answer: 1, or 0 */

/** open's mode flags. */
constexpr uint64_t kOpenCreate = 1;   /* no such name: make the file */
constexpr uint64_t kOpenTruncate = 2; /* an old chain is freed at open */

/** The most data one write call carries: the envelope's words, less the
 *  handle and the count, in bytes. The same bound as a read's answer, for
 *  the same reason. */
constexpr uint32_t kWriteMax = 117 * 8;

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

/** stat's answer: the entry's kind and its size, in that order. */
constexpr uint32_t kStatTailWords = 2;

}  // namespace aegir::volume
