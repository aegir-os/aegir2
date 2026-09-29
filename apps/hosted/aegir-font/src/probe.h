/*
 * Reading a face's own name without a rasterizer (specs/fonts.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The font service indexes `Sys:Fonts` by opening each file and asking who it
 * is. FreeType can answer, but only by opening the face, and a face in the CJK
 * collection is CFF-based: opening one reads megabytes through a VFS window of
 * 936 bytes, and indexing sixteen of them cost twenty seconds of a boot. The
 * name lives in a few kilobytes, so it is read directly here and FreeType is
 * left the rendering (specs/fonts.md).
 *
 * Two formats carry the name where a reader can find it:
 *
 *   - a BDF's properties are text at the very top of the file;
 *   - a TrueType/OpenType face's is in its `name` table, reached through the
 *     table directory, and a collection (`.ttc`) is a directory per face.
 *
 * The reader is a function and a context, not a file, so the probe is pure
 * given a reader and the host conformance (scripts/check_font_probe.py) drives
 * it over the real faces from the vendored trees.
 */

#ifndef AEGIR_FONT_PROBE_H
#define AEGIR_FONT_PROBE_H

#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace aegir::font {

/** Random access to a file, the one thing a probe needs of one. False when the
 *  read cannot be satisfied (past the end, or the file refused). */
struct Reader {
    void *context = nullptr;
    uint64_t size = 0;
    bool (*at)(void *context, uint64_t offset, uint64_t length, void *out) = nullptr;
};

/** One face a file holds, by the file's own metadata. `style` is the weight and
 *  slant a request is matched against ("Bold", "Italic", "Bold Italic",
 *  "Medium"); a bitmap-only face is not scalable and carries the pixel size it
 *  has. */
struct FaceInfo {
    std::string family;
    std::string style;
    bool scalable = true;
    int pixel_size = 0;
    /* The face's own index within its file: zero for a single-face file, its
     * position in the collection for a `.ttc` -- which is what opening it
     * again expects. */
    uint32_t index = 0;
};

/** Read a BDF's header for its family, weight, slant and pixel size. Pure: the
 *  header is a prefix of the file. True when it named a family. */
bool probe_bdf(std::string_view header, FaceInfo *out);

/** Every face the file holds, by its own metadata, or empty when it is not a
 *  face this probe knows -- which is not an error (specs/fonts.md). A TrueType
 *  or OpenType face is scalable; a collection contributes one entry per face. */
std::vector<FaceInfo> probe_file(Reader const &reader);

}  // namespace aegir::font

#endif  // AEGIR_FONT_PROBE_H
