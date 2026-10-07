/*
 * aegir-posix: the path view, as a pure value (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The POSIX view hangs every volume and every binding off `/`: `/Name/rest` is
 * the native `Name:rest`, `/` is the synthetic root, and `.` and `..` are
 * normalized here because the Amiga's directories have none (specs/vfs.md).
 * The translation is one step at the files boundary -- a path that begins with
 * `/` is POSIX and is rewritten, anything else is native and passes through --
 * so it is a pure function of the path and the current directory: no VFS call,
 * no state, and no buffer of its own. The caller brings the room.
 *
 * That is what makes it testable without a boot (scripts/check_posix_path.py,
 * `make check-posix-path`): the rules are the spec's table, and they are
 * asserted directly. It includes no Aegir or seL4 header for the same reason,
 * and it names no maximum length -- the bound is the caller's buffer, so the
 * namespace protocol's own ceiling is the only one in the picture.
 */

#ifndef AEGIR_POSIX_PATH_H
#define AEGIR_POSIX_PATH_H

#include <stdint.h>

namespace aegir::posix::path {

namespace detail {

/** Whether a path names a volume or binding before any slash: Aegir's absolute
 *  shape is `Volume:rest`, and a `/` first means it is a POSIX path instead.
 *  (files.cc asked the same question before the view landed; it is the
 *  translation's own test now, so the boundary asks it once.) */
inline bool names_a_volume(char const *path, uint32_t length) noexcept
{
    for (uint32_t i = 0; i < length; ++i) {
        if (path[i] == ':') {
            return true;
        }
        if (path[i] == '/') {
            return false;
        }
    }
    return false;
}

}  // namespace detail

/** Where a translation lands: a path the VFS can resolve, or the synthetic
 *  root, which no VFS path names. */
enum class Target : uint8_t {
    Vfs, /* `vfs` holds the `Volume:rest` path to resolve */
    Root /* `/`: every volume and binding (specs/posix.md) */
};

/** What `translate` answered: where the path landed, and how many bytes of the
 *  caller's buffer it wrote (zero for the root). */
struct Translation {
    Target target;
    uint32_t length;
};

/**
 * Translate one path a program handed the layer into the path Aegir resolves.
 *
 * `cwd` is the layer's current directory in its POSIX form -- `/Sys/Tests`, or
 * `/` for the root -- and empty when the process has none, which is what makes
 * a relative path refused rather than guessed at (specs/environment.md).
 *
 * A path that names a volume or binding before any slash (`Sys:DOCS/X`) is
 * native and passes through unchanged: that is the grammar `std::filesystem`
 * speaks, and nothing native changes (specs/posix.md's boundary). Everything
 * else is POSIX. A leading `/` is absolute and anything else is composed with
 * `cwd`; then `.` is dropped and `..` resolved against what is left, where
 * `..` above a volume's root is `/` and `..` at `/` is `/`. The first
 * component names a volume or a binding and is kept exactly as written: folding
 * its case is the namespace's (specs/vfs.md), and below it a filesystem's own
 * rule applies.
 *
 * Returns false when there is nothing to translate (an empty path, or a
 * relative one with no current directory) or when the answer does not fit
 * `capacity` bytes. On success `out.target` says what `vfs` holds.
 */
inline bool translate(char const *path, uint32_t length, char const *cwd,
                      uint32_t cwd_length, char *vfs, uint32_t capacity,
                      Translation &out) noexcept
{
    if (path == nullptr || vfs == nullptr || length == 0) {
        return false;
    }
    if (detail::names_a_volume(path, length)) {
        /* Native: as written, so a path the caller composed in Aegir's own
         * grammar -- `std::filesystem`, the shell, a redirect -- is untouched. */
        if (length > capacity) {
            return false;
        }
        for (uint32_t i = 0; i < length; ++i) {
            vfs[i] = path[i];
        }
        out.target = Target::Vfs;
        out.length = length;
        return true;
    }

    /* POSIX: absolute as written, or composed with the current directory. */
    uint32_t used = 0;
    if (path[0] == '/') {
        if (length > capacity) {
            return false;
        }
        for (; used < length; ++used) {
            vfs[used] = path[used];
        }
    } else {
        if (cwd == nullptr || cwd_length == 0 || cwd_length + 1 + length > capacity) {
            return false;
        }
        for (uint32_t i = 0; i < cwd_length; ++i) {
            vfs[used++] = cwd[i];
        }
        vfs[used++] = '/';
        for (uint32_t i = 0; i < length; ++i) {
            vfs[used++] = path[i];
        }
    }

    /* Normalize in place, component by component: the kept ones are rebuilt
     * from the front, and the write cursor can never pass the read cursor
     * because dropping, popping or collapsing only ever makes the result
     * shorter than what has been read. */
    uint32_t read = 0;
    uint32_t write = 0;
    uint32_t kept = 0;
    while (read < used) {
        while (read < used && vfs[read] == '/') {
            ++read; /* an empty component: `/` is a separator, not a name */
        }
        uint32_t const start = read;
        while (read < used && vfs[read] != '/') {
            ++read;
        }
        uint32_t const component = read - start;
        if (component == 0) {
            continue;
        }
        if (component == 1 && vfs[start] == '.') {
            continue; /* `.` is the directory itself, and the Amiga has none */
        }
        if (component == 2 && vfs[start] == '.' && vfs[start + 1] == '.') {
            if (kept != 0) {
                /* Pop the component this one resolves past: the result ends
                 * where the separator before it began -- at the root when
                 * there is nothing left to pop, which is `..` at `/`. */
                --kept;
                while (write > 0 && vfs[write - 1] != '/') {
                    --write;
                }
                if (write > 0) {
                    --write; /* the separator that introduced it */
                }
            }
            continue;
        }
        if (kept != 0) {
            vfs[write++] = '/';
        }
        for (uint32_t i = 0; i < component; ++i) {
            vfs[write++] = vfs[start + i];
        }
        ++kept;
    }

    /* `/dev/null` is Aegir's `NIL:` -- the device whose reads are EOF and whose
     * writes are dropped (specs/vfs.md) -- which is exactly what POSIX means by
     * it, and which a compiler asks for: LLVM opens /dev/null to decide whether
     * its output has colours (projects/llvm-project/llvm/lib/Support/Unix/
     * Process.inc:231), so printing needs it. Nothing here binds a `dev:` volume,
     * so the view answers it the way it answers every other POSIX path: the
     * volume's name is folded as the namespace folds any first component, and the
     * name below it is kept as written. `NIL:` takes any name (specs/pipe.md: "a
     * read is EOF, a write is dropped"), so `NIL:null` is the device. */
    if (write == 8) {
        static char const kDevice[] = "dev/null";
        static char const kNative[] = "NIL:null";
        bool matches = true;
        for (uint32_t i = 0; i < 8 && matches; ++i) {
            char c = vfs[i];
            if (i < 3 && c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            matches = c == kDevice[i];
        }
        if (matches) {
            if (sizeof(kNative) > capacity) {
                return false;
            }
            for (uint32_t i = 0; i < sizeof(kNative); ++i) {
                vfs[i] = kNative[i];
            }
            out.target = Target::Vfs;
            out.length = sizeof(kNative) - 1;
            return true;
        }
    }

    /* What is left is `Name` or `Name/rest`: the root is its own answer, and
     * the first separator becomes the colon that names the volume's root. */
    if (kept == 0) {
        out.target = Target::Root;
        out.length = 0;
        return true;
    }
    uint32_t separator = 0;
    while (separator < write && vfs[separator] != '/') {
        ++separator;
    }
    if (separator == write) {
        if (write == capacity) {
            return false; /* the colon that names the root has nowhere to go */
        }
        vfs[write++] = ':';
    } else {
        vfs[separator] = ':';
    }
    out.target = Target::Vfs;
    out.length = write;
    return true;
}

/**
 * The other direction: the VFS path `Sys:Tests` as the POSIX view's
 * `/Sys/Tests`, which is what `getcwd` answers and what a relative path is
 * composed against. The name is the one the path carries, as written: a
 * resolve through an alias presents the alias (specs/posix.md's listing rule).
 *
 * Returns false when the path names no volume (no colon, or nothing before
 * one) -- it is not a VFS path at all -- or when it does not fit `capacity`.
 */
inline bool present(char const *vfs, uint32_t vfs_length, char *out, uint32_t capacity,
                    uint32_t &out_length) noexcept
{
    if (vfs == nullptr || out == nullptr) {
        return false;
    }
    uint32_t colon = 0;
    while (colon < vfs_length && vfs[colon] != ':') {
        ++colon;
    }
    if (colon == 0 || colon == vfs_length) {
        return false;
    }
    uint32_t const rest = vfs_length - colon - 1;
    uint32_t const total = 1 + colon + (rest != 0 ? 1 + rest : 0);
    if (total > capacity) {
        return false;
    }
    uint32_t write = 0;
    out[write++] = '/';
    for (uint32_t i = 0; i < colon; ++i) {
        out[write++] = vfs[i];
    }
    if (rest != 0) {
        out[write++] = '/';
        for (uint32_t i = 0; i < rest; ++i) {
            out[write++] = vfs[colon + 1 + i];
        }
    }
    out_length = write;
    return true;
}

}  // namespace aegir::posix::path

#endif  // AEGIR_POSIX_PATH_H
