/*
 * The protocol the VFS's namespace port serves, and nothing else.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The VFS owns the namespace -- volume names, path resolution, who may look
 * up what -- and not the data (specs/vfs.md). Files live in the filesystems;
 * this port is the map to them. Three shapes of question:
 *
 *   - `register`: a new volume. Words carry the name and flags; one
 *     capability rides beside them -- the volume port's caller half,
 *     *unbadged*, because a badged cap cannot be re-minted and the VFS mints
 *     each resolver's own badge onto the copy it hands out. The answer is
 *     the name the volume actually got: a duplicate gains a `_N` suffix, and
 *     the registrant is told rather than shadowed silently.
 *   - `resolve`: a path, `Volume:rest`, where the volume part may be an
 *     alias (specs/vfs.md's Aliases). The answer is one capability -- the
 *     volume's port, minted with the caller's badge, so the filesystem sees
 *     the true caller -- and the volume-relative path the argument truly
 *     names, as a string: substitution composes a rest the caller's string
 *     never contained, and one reply shape serves both. Everything after
 *     the colon is the filesystem's to interpret.
 *   - `bind`: an alias for one badge -- the badge, the name, the path it
 *     stands for. auth binds each session's Home: at login.
 *   - `count`/`describe`: the volumes, one Row per describe. The registry
 *     pattern (aegir/registry.h) applied to names.
 *
 * Strings travel as words, first word a length, the bytes after it -- a path
 * is short by nature, and the envelope's ceiling (aegir/ipc) is the refusal
 * bound. A method the port does not know is answered by saying nothing.
 */

#pragma once

#include <stdint.h>

namespace aegir::nmspace {

/** The port's name, in the class.instance shape every port is named in. */
constexpr char kPortName[] = "vfs.namespace";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

constexpr uint32_t kMethodRegister = 1; /* in: name words + 1 cap; answer: assigned name */
constexpr uint32_t kMethodResolve = 2;  /* in: path words; answer: the rest string + 1 cap */
constexpr uint32_t kMethodCount = 3;    /* answer: how many volumes the namespace holds */
constexpr uint32_t kMethodDescribe = 4; /* in: an index; answer: a Row's words */
constexpr uint32_t kMethodBind = 5;     /* in: badge, flags, name, path; answer: 1 bound, 0 refused */
/* unbind: a badge. Every binding the badge holds is dropped -- the session
 * teardown's mechanism, beside the volume protocol's reap (specs/vfs.md).
 * Answer: how many were dropped. */
constexpr uint32_t kMethodUnbind = 6;   /* in: badge; answer: how many */

/** Bind flags (specs/namespace.md). A second bind of the same name appends or
 *  prepends a member -- the union, a name read as an ordered list of
 *  directories; the default replaces the binding, which is the one-member
 *  alias specs/vfs.md describes. `kBindCreate` marks the member a create
 *  lands in; at most one member of a union should carry it. */
constexpr uint64_t kBindAppend = 1;
constexpr uint64_t kBindPrepend = 2;
constexpr uint64_t kBindCreate = 4;

/** Register flags. */
constexpr uint64_t kFlagReadOnly = 1;
constexpr uint64_t kFlagBoot = 2; /* the system volume -- the VFS aliases it Sys: */

/** The longest name a volume may carry, NUL not included: short enough to
 *  travel in the envelope with room for what comes after it, long enough for
 *  a filesystem label with a `_N` suffix. */
constexpr uint32_t kNameMax = 24;

/** The longest path a resolve may carry: what fits the envelope after the
 *  length word (aegir/ipc's kMaxWords words, less one). A path longer than
 *  that is a protocol that wants the buffer form, when it exists. */
constexpr uint32_t kPathMax = 944;

/** A string on the wire: the byte count, then the bytes, packed into words.
 *  These helpers are the wire's one shape, shared by name, path and answer.
 *  `max` is what the *field* allows -- kNameMax for a volume name, the
 *  envelope's own room for a path -- and a string past it does not travel. */
inline uint32_t pack_string(uint64_t *out, char const *text, uint32_t length,
                            uint32_t max) noexcept
{
    if (length > max) {
        return 0;
    }
    out[0] = length;
    char *bytes = reinterpret_cast<char *>(out + 1);
    for (uint32_t i = 0; i < length; ++i) {
        bytes[i] = text[i];
    }
    uint32_t const words = 1 + (length + 7) / 8;
    if (length % 8 != 0) {
        out[words - 1] &= ~(~0ULL << ((length % 8) * 8));
    }
    return words;
}

/** The reverse of pack_string: `in` holds {length, bytes...}; the view lands
 *  in `text`/`length`. False when the wire lies about its own length. */
inline bool unpack_string(uint64_t const *in, uint32_t words, uint32_t max,
                          char const **text, uint32_t *length) noexcept
{
    uint64_t const n = in[0];
    if (n > max || words < 1 + (n + 7) / 8) {
        return false;
    }
    *text = reinterpret_cast<char const *>(in + 1);
    *length = static_cast<uint32_t>(n);
    return true;
}

/** One volume, as the namespace knows it. */
struct Row {
    char name[kNameMax]; /* NUL-terminated within the field */
    uint64_t flags;      /* kFlagReadOnly and friends */
    uint64_t bound;      /* 1 when a filesystem's cap is held for it */
};

/** The Row as the message carries it. */
constexpr uint32_t kRowWords = (sizeof(Row) + 7) / 8;

/* resolve's answer, at its longest: the volume-relative rest as one packed
 * string -- the whole envelope, the way the request's path is. */
constexpr uint32_t kResolveWords = kPathMax / 8 + 1;

}  // namespace aegir::nmspace
