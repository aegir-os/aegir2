/*
 * Trinket Font system.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Bitmap fonts with fallback chains. Supports Terminus (primary) +
 * Noto Sans fallbacks for CJK, Arabic, Hebrew, etc.
 */

#ifndef AEGIR_TRINKET_FONT_H
#define AEGIR_TRINKET_FONT_H

#include <aegir/trinket/bidi.h>
#include <aegir/trinket/color.h>
#include <aegir/trinket/locale.h>
#include <aegir/trinket/point.h>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aegir::trinket {

struct Glyph {
    int advance = 0;           // Logical pixels
    int bearing_x = 0;
    int bearing_y = 0;
    int width = 0;
    int height = 0;
    int atlas_x = 0;
    int atlas_y = 0;
    bool valid = false;
};

struct PositionedGlyph {
    Glyph glyph;
    int x = 0;
    int y = 0;
    uint32_t cluster = 0;  // For BiDi mapping
};

class BitmapFont;

class Font {
public:
    virtual ~Font() = default;

    // Font metrics
    virtual int height() const = 0;        // Line height (logical px)
    virtual int ascent() const = 0;
    virtual int descent() const = 0;
    virtual int line_gap() const = 0;

    // Glyph lookup
    virtual const Glyph* glyph(uint32_t codepoint) const = 0;
    virtual const Glyph* find_glyph(uint32_t codepoint) const;

    // The bitmap atlas this font draws from, when it is a bitmap font, and null
    // otherwise. The canvas blits through this rather than casting a Font* to a
    // BitmapFont*: a fallback chain or a future outline font is not one, and the
    // cast is undefined behaviour when the glyph is not.
    virtual const BitmapFont* as_bitmap() const { return nullptr; }

    // Text measurement (simple, no shaping)
    virtual Size measure(std::u32string_view text) const;
    virtual Size measure(std::string_view utf8) const;

    // Text shaping (Phase 2: HarfBuzz)
    virtual std::vector<PositionedGlyph> shape(std::u32string_view text,
                                               BidiDirection dir = BidiDirection::LTR) const;

    // Fallback chain
    void add_fallback(std::unique_ptr<Font> font);
    const std::vector<Font*>& fallbacks() const { return fallbacks_; }

    // Factory
    static std::unique_ptr<Font> load_terminus(int size_pts, float scale);
    static std::unique_ptr<Font> load_terminus_bold(int size_pts, float scale);
    static std::unique_ptr<Font> create_with_fallbacks(std::string_view family,
                                                        int size_pts, float scale,
                                                        const Locale& locale);

    // Get default fallback families for a locale
    static std::vector<std::string> fallback_families_for_locale(const Locale& locale);

protected:
    std::vector<std::unique_ptr<Font>> owned_fallbacks_;
    std::vector<Font*> fallbacks_;
};

class BitmapFont : public Font {
public:
    BitmapFont() = default;
    ~BitmapFont() override;

    // Load from Terminus .bdf or .pcf
    bool load_bdf(const void* data, size_t size);
    bool load_pcf(const void* data, size_t size);

    // Font metrics
    int height() const override { return height_; }
    int ascent() const override { return ascent_; }
    int descent() const override { return descent_; }
    int line_gap() const override { return line_gap_; }

    const Glyph* glyph(uint32_t codepoint) const override;
    const BitmapFont* as_bitmap() const override { return this; }

    // Atlas access for rendering
    struct Atlas {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels;  // 8-bit alpha
    };
    const Atlas& atlas() const { return atlas_; }

protected:
    /* Lay one glyph's coverage into the atlas and record it: a row of its own,
     * with the atlas widened when a wider glyph arrives (specs/fonts.md). A
     * subclass whose glyphs are rasterized one at a time -- ServerFont, over
     * the font service -- fills its atlas this way; `load_bdf` lays its whole
     * face out in one pass instead. False when the atlas could not grow. */
    bool adopt_glyph(uint32_t codepoint, const Glyph& glyph, const uint8_t* bits);

    /* The atlas and the glyphs a subclass fills. `glyph_map_` is a map and not
     * a sorted vector because a pointer it hands out must survive the next
     * glyph's arrival: a vector's growth would dangle it. */
    int height_ = 0;
    int ascent_ = 0;
    int descent_ = 0;
    int line_gap_ = 0;
    Atlas atlas_;
    std::map<uint32_t, Glyph> glyph_map_;

private:
    // BDF parse state: ENCODING and BBX arrive before the BITMAP lines that
    // belong to them, so they are carried between lines.
    uint32_t current_encoding_ = 0;
    Glyph current_glyph_{};

    static int hex_val(char c) noexcept;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_FONT_H