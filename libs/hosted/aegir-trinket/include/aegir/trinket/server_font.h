/*
 * Trinket ServerFont: a face drawn through the font service (specs/fonts.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The toolkit has one `Font` seam (aegir/trinket/font.h): `BitmapFont` parses
 * a BDF in the app, and `ServerFont` is a client of `font.main`, so a widget
 * draws a TrueType, OpenType or collection face without the app carrying a
 * rasterizer or a face set. The face lives in the service; a glyph is
 * rasterized on demand into a page this client owns, and copied from there
 * into the client's own atlas, which is what the canvas blits from.
 */

#ifndef AEGIR_TRINKET_SERVER_FONT_H
#define AEGIR_TRINKET_SERVER_FONT_H

#include <aegir/trinket/font.h>
#include <aegir/ipc/port.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <memory>
#include <string_view>
#include <vector>

namespace aegir::trinket {

class ServerFont : public BitmapFont {
public:
    /** Open `family` at `size` through `service`, taking one transfer page from
     *  `allocator` and `scratch`. Null when the process was given no port, the
     *  service has no such face, or the page cannot be made -- the caller falls
     *  back, and says so. */
    static std::unique_ptr<ServerFont> open(aegir::ipc::Consumer service,
                                            aegir::mem::Allocator& allocator,
                                            aegir::mem::Scratch& scratch,
                                            std::string_view family, int size,
                                            bool bold, bool italic);
    ~ServerFont() override;

    /** The glyph, fetched from the service on first use and cached. A font
     *  whose glyphs are all asked for in one run -- a label measures its text,
     *  and the measure prefetches -- pays one call per batch, not per glyph. */
    const Glyph* glyph(uint32_t codepoint) const override;

    /** Measure, but fetch the run's missing codepoints first: the batch the
     *  protocol carries is used where the caller knows what is coming. */
    Size measure(std::u32string_view text) const override;

private:
    ServerFont() = default;

    /** Ask the service for the first `kBatch` of `codes` and adopt what it
     *  answers. `answered`, when given, is how many codepoints it got to -- an
     *  answer is a prefix of the request when the page filled. False only when
     *  the service refused or the answer was malformed. */
    bool fetch(std::vector<uint32_t> const& codes, uint32_t* answered);

    /** Fetch until `missing` is empty, dropping the codepoints each answer
     *  covered. False when the service stopped answering: the rest stay
     *  unasked, and `glyph` records them as misses rather than calling again
     *  on every draw. */
    bool drain(std::vector<uint32_t>& missing);

    aegir::ipc::Consumer service_;
    uint64_t face_id_ = 0;
    /* The page: the capability the client maps and reads, and the unmapped
     * copy handed to the service for each call (specs/fonts.md). */
    seL4_CPtr frame_ = 0;
    seL4_CPtr send_ = 0;
    uint8_t* page_ = nullptr;
    /* A service that would not answer is said once, not once per glyph. */
    bool reported_ = false;
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_SERVER_FONT_H
