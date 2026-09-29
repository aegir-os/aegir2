/*
 * Trinket font catalog: the faces `Sys:Fonts` holds (specs/fonts.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The system volume's fonts are found by scanning, not by a configured list: a
 * face is indexed by its *own* name (a BDF header's FAMILY_NAME, WEIGHT_NAME,
 * SLANT and PIXEL_SIZE), so the tree's shape is for a person navigating it and
 * a face dropped anywhere under the root is found. The scan reads a header,
 * not a face -- the CJK collection beside it is megabytes.
 *
 * The two halves that need no filesystem -- parsing a BDF header, and choosing
 * a face from a list -- are pure, so the host conformance
 * (scripts/check_fonts.py) asserts them.
 */

#ifndef AEGIR_TRINKET_FONTS_H
#define AEGIR_TRINKET_FONTS_H

#include <string>
#include <string_view>
#include <vector>

namespace aegir::trinket {

/** One face a `Sys:Fonts` scan found (specs/fonts.md): the face's own
 *  metadata, and where it sits. A tree's shape is not an index -- the path is
 *  for a person -- so a face is found by its family, weight, slant and size. */
struct FontFace {
    std::string family;
    std::string weight;  // "Medium", "Bold", ...
    std::string slant;   // "R" upright, "I" italic, "O" oblique
    int pixel_size = 0;
    std::string path;

    bool bold() const { return weight == "Bold" || weight == "Black" || weight == "Demi"; }
    bool italic() const { return slant == "I" || slant == "O"; }
};

/** Read a BDF header for the face's own name (specs/fonts.md): FAMILY_NAME,
 *  WEIGHT_NAME, SLANT and PIXEL_SIZE, from a prefix of the file. Pure, so the
 *  host conformance asserts it (scripts/check_fonts.py). True when the header
 *  named a family. */
bool probe_bdf(std::string_view header, FontFace* out);

/** The face a request picks from a list (specs/fonts.md): the family's, then
 *  the size nearest the ask, then the style nearest it. Null when the family
 *  is absent -- the caller falls back. */
FontFace const* select_face(std::vector<FontFace> const& faces, std::string_view family,
                            int pixel_size, bool bold, bool italic);

/** A whole file's bytes, or empty when it cannot be read. The face a catalog
 *  picked is loaded with this; the toolkit's one file read. */
std::string read_file(std::string const& path);

/** The faces a `Sys:Fonts` scan found. */
class FontCatalog {
public:
    /** Scan `root` recursively. A file that is not a face the catalog can
     *  read, or whose header names no family, is skipped -- not fatal. True
     *  when the scan found at least one face. */
    bool scan(std::string_view root);

    std::vector<FontFace> const& faces() const { return faces_; }

    /** The best face for a request, or null. */
    FontFace const* find(std::string_view family, int pixel_size, bool bold,
                         bool italic) const {
        return select_face(faces_, family, pixel_size, bold, italic);
    }

private:
    std::vector<FontFace> faces_;
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_FONTS_H
