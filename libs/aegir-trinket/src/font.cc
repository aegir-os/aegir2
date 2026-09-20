/*
 * Trinket Font implementation.
 */

#include <aegir/trinket/font.h>
#include <aegir/trinket/unicode.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace aegir::trinket {

const Glyph* Font::find_glyph(uint32_t codepoint) const {
    const Glyph* g = glyph(codepoint);
    if (g && g->valid) return g;
    for (Font* fb : fallbacks_) {
        g = fb->glyph(codepoint);
        if (g && g->valid) return g;
    }
    return nullptr;
}

Size Font::measure(std::u32string_view text) const {
    int width = 0;
    for (char32_t cp : text) {
        const Glyph* g = find_glyph(cp);
        if (g) width += g->advance;
    }
    return {width, height()};
}

Size Font::measure(std::string_view utf8) const {
    return measure(utf8_to_utf32(utf8));
}

std::vector<PositionedGlyph> Font::shape(std::u32string_view text,
                                          BidiDirection dir) const {
    static_cast<void>(dir);  // shaping direction is the BiDi arc's
    std::vector<PositionedGlyph> result;
    int x = 0;
    for (char32_t cp : text) {
        const Glyph* g = find_glyph(cp);
        if (g && g->valid) {
            PositionedGlyph pg;
            pg.glyph = *g;
            pg.x = x + g->bearing_x;
            pg.y = -g->bearing_y;
            result.push_back(pg);
            x += g->advance;
        }
    }
    return result;
}

void Font::add_fallback(std::unique_ptr<Font> font) {
    fallbacks_.push_back(font.get());
    owned_fallbacks_.push_back(std::move(font));
}

std::vector<std::string> Font::fallback_families_for_locale(const Locale& locale) {
    std::vector<std::string> families;
    std::string lang = locale.language();

    if (lang == "zh" || lang == "ja" || lang == "ko") {
        families = {"Noto Sans CJK", "Noto Sans SC", "Noto Sans TC", "Noto Sans JP", "Noto Sans KR"};
    } else if (lang == "ar" || lang == "fa" || lang == "ur") {
        families = {"Noto Sans Arabic", "Noto Naskh Arabic"};
    } else if (lang == "he") {
        families = {"Noto Sans Hebrew"};
    } else if (lang == "hi" || lang == "mr" || lang == "ne") {
        families = {"Noto Sans Devanagari"};
    } else if (lang == "th") {
        families = {"Noto Sans Thai"};
    }
    families.push_back("Noto Sans");  // Universal fallback
    return families;
}

// BitmapFont implementation
BitmapFont::~BitmapFont() = default;

bool BitmapFont::load_bdf(const void* data, size_t size) {
    // Simple BDF parser - minimal implementation for Terminus fonts
    const char* ptr = static_cast<const char*>(data);
    const char* end = ptr + size;

    // Parse header
    while (ptr < end) {
        const char* line_end = static_cast<const char*>(memchr(ptr, '\n', end - ptr));
        if (!line_end) line_end = end;
        size_t line_len = line_end - ptr;

        if (line_len >= 10 && memcmp(ptr, "FONTBOUNDINGBOX ", 16) == 0) {
            int w, h, xoff, yoff;
            sscanf(ptr + 16, "%d %d %d %d", &w, &h, &xoff, &yoff);
            atlas_.width = w;
            atlas_.height = h;
        } else if (line_len >= 5 && memcmp(ptr, "CHARS", 5) == 0) {
            // Skip
        } else if (line_len >= 9 && memcmp(ptr, "STARTCHAR", 9) == 0) {
            // Parse character
            char name[64];
            sscanf(ptr + 9, "%63s", name);
        } else if (line_len >= 8 && memcmp(ptr, "ENCODING", 8) == 0) {
            uint32_t encoding;
            sscanf(ptr + 8, "%u", &encoding);
            current_encoding_ = encoding;
        } else if (line_len >= 6 && memcmp(ptr, "BBX ", 4) == 0) {
            int w, h, xoff, yoff;
            sscanf(ptr + 4, "%d %d %d %d", &w, &h, &xoff, &yoff);
            current_glyph_ = Glyph{};
            current_glyph_.width = w;
            current_glyph_.height = h;
            current_glyph_.bearing_x = xoff;
            current_glyph_.bearing_y = yoff + h;  // BDF y is from baseline
            current_glyph_.valid = (w > 0 && h > 0);
        } else if (line_len >= 7 && memcmp(ptr, "BITMAP", 6) == 0) {
            // Read bitmap rows
            if (current_glyph_.valid) {
                int bytes_per_row = (current_glyph_.width + 7) / 8;
                size_t offset = atlas_.pixels.size();
                atlas_.pixels.resize(offset + current_glyph_.height * atlas_.width);

                for (int y = 0; y < current_glyph_.height; ++y) {
                    ptr = line_end + 1;
                    line_end = static_cast<const char*>(memchr(ptr, '\n', end - ptr));
                    if (!line_end) line_end = end;
                    line_len = line_end - ptr;
                    if (line_len >= static_cast<size_t>(bytes_per_row) * 2) {
                        // Parse hex bytes
                        for (int x = 0; x < current_glyph_.width; ++x) {
                            int byte_idx = x / 8;
                            int bit = 7 - (x % 8);
                            if (byte_idx * 2 + 1 < static_cast<int>(line_len)) {
                                char hi = ptr[byte_idx * 2];
                                char lo = ptr[byte_idx * 2 + 1];
                                int val = (hex_val(hi) << 4) | hex_val(lo);
                                if (val & (1 << bit)) {
                                    int tx = current_glyph_.bearing_x + x;
                                    int ty = current_glyph_.bearing_y - y;
                                    if (tx >= 0 && tx < static_cast<int>(atlas_.width) &&
                                        ty >= 0 && ty < static_cast<int>(atlas_.height)) {
                                        atlas_.pixels[ty * atlas_.width + tx] = 255;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else if (line_len >= 7 && memcmp(ptr, "ENDCHAR", 7) == 0) {
            if (current_glyph_.valid) {
                glyph_map_.emplace_back(current_encoding_, current_glyph_);
            }
        } else if (line_len >= 7 && memcmp(ptr, "FONT_ASCENT", 11) == 0) {
            sscanf(ptr + 11, "%d", &ascent_);
        } else if (line_len >= 9 && memcmp(ptr, "FONT_DESCENT", 12) == 0) {
            sscanf(ptr + 12, "%d", &descent_);
        } else if (line_len >= 9 && memcmp(ptr, "FONT_HEIGHT", 11) == 0) {
            sscanf(ptr + 11, "%d", &height_);
        }

        ptr = line_end + 1;
    }

    // Sort glyph map for binary search
    std::sort(glyph_map_.begin(), glyph_map_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    height_ = ascent_ + descent_;
    return true;
}

bool BitmapFont::load_pcf(const void* data, size_t size) {
    // PCF parsing would go here
    static_cast<void>(data);
    static_cast<void>(size);
    return false;
}

const Glyph* BitmapFont::glyph(uint32_t codepoint) const {
    auto it = std::lower_bound(glyph_map_.begin(), glyph_map_.end(), codepoint,
                               [](const auto& a, uint32_t val) { return a.first < val; });
    if (it != glyph_map_.end() && it->first == codepoint) {
        return &it->second;
    }
    return nullptr;
}

// Helpers
int BitmapFont::hex_val(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

std::unique_ptr<Font> Font::load_terminus(int size_pts, float scale) {
    // Will load from resources/fonts/terminus/
    static_cast<void>(size_pts);
    static_cast<void>(scale);
    return nullptr;  // TODO: Implement resource loading
}

std::unique_ptr<Font> Font::load_terminus_bold(int size_pts, float scale) {
    static_cast<void>(size_pts);
    static_cast<void>(scale);
    return nullptr;  // TODO
}

std::unique_ptr<Font> Font::create_with_fallbacks(std::string_view family,
                                                   int size_pts, float scale,
                                                   const Locale& locale) {
    static_cast<void>(family);  // family selection is a later milestone
    auto primary = load_terminus(size_pts, scale);
    if (!primary) return nullptr;

    // The fallback chain is built here when the font resources land.
    static_cast<void>(fallback_families_for_locale(locale));
    return primary;
}

} // namespace aegir::trinket