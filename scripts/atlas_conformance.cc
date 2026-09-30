/*
 * Host conformance for the toolkit's glyph atlas (specs/fonts.md).
 *
 * A glyph the font service rasterizes is copied into the client's own atlas by
 * BitmapFont::adopt_glyph, and the canvas reads it back at the glyph's
 * atlas_x/atlas_y -- `pixels[(atlas_y + y) * atlas.width + atlas_x + x]`. The
 * two must agree, and the atlas grows: a glyph wider than the rows placed so
 * far widens every row, which moves rows a glyph already occupies.
 *
 * This asserts the copy with synthetic rasters -- a byte no other pixel shares,
 * so any misplacement is visible -- and reads the atlas exactly the way
 * Canvas::draw_text does. It is pure: no filesystem, no rasterizer.
 *
 * Host tools only (python3 and a C++ compiler); not part of any target build.
 * Run through scripts/check_atlas.py.
 */

#include <aegir/trinket/font.h>
#include <aegir/trinket/locale.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace aegir::trinket {
/* The atlas does not read the locale; fallback_families_for_locale names it, so
 * a stub satisfies the linker. */
std::string Locale::language() const { return {}; }
}  // namespace aegir::trinket

/* font.cc's load_terminus names the embedded Terminus; the atlas does not use
 * it, so a stub satisfies the linker. The shape is what embed_binary.py emits. */
extern const unsigned char terminus_12_bdf[] = {0};
extern const unsigned long terminus_12_bdf_size = 1;

namespace {

using aegir::trinket::BitmapFont;
using aegir::trinket::Glyph;

/* adopt_glyph is the subclass seam; a probe exposes it. */
class Probe : public BitmapFont {
public:
    bool adopt(uint32_t codepoint, const Glyph& glyph, const uint8_t* bits) {
        return adopt_glyph(codepoint, glyph, bits);
    }
};

unsigned g_checks = 0;
unsigned g_failures = 0;

void check(bool ok, std::string const& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

/* A raster where each pixel's value is its own: a placement that reads the
 * wrong row, stride, or offset shows up as a mismatch. */
std::vector<uint8_t> raster(int width, int height) {
    std::vector<uint8_t> bits(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bits[static_cast<size_t>(y) * width + x] =
                static_cast<uint8_t>(1 + ((y * 31 + x * 7) % 254));
        }
    }
    return bits;
}

Glyph make_glyph(int width, int height, int advance) {
    Glyph glyph;
    glyph.width = width;
    glyph.height = height;
    glyph.advance = advance;
    glyph.bearing_x = 0;
    glyph.bearing_y = height;
    glyph.valid = true;
    return glyph;
}

/* The atlas as the canvas reads it. */
void expect_placed(Probe const& font, uint32_t codepoint, int width, int height,
                   std::vector<uint8_t> const& bits) {
    char const* const name = "a glyph";
    Glyph const* const glyph = font.glyph(codepoint);
    if (glyph == nullptr) {
        check(false, std::string(name) + " is missing from the atlas");
        return;
    }
    check(glyph->width == width && glyph->height == height,
          std::string(name) + "'s box is kept");
    auto const& atlas = font.atlas();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t const at =
                static_cast<size_t>(glyph->atlas_y + y) * atlas.width + (glyph->atlas_x + x);
            bool const ok =
                at < atlas.pixels.size() &&
                atlas.pixels[at] == bits[static_cast<size_t>(y) * width + x];
            if (!ok) {
                check(false, std::string(name) + ": atlas (" + std::to_string(x) + "," +
                                 std::to_string(y) + ") is wrong for a " + std::to_string(width) +
                                 "x" + std::to_string(height) + " glyph at (" +
                                 std::to_string(glyph->atlas_x) + "," +
                                 std::to_string(glyph->atlas_y) + "), atlas " +
                                 std::to_string(atlas.width) + "x" + std::to_string(atlas.height));
                return;
            }
        }
    }
    ++g_checks;
}

}  // namespace

int main() {
    Probe font;

    /* A narrow glyph, a second one, and then a wider one: widening moves every
     * row, so the first two must move with it. */
    constexpr int kFirstWidth = 11;
    constexpr int kFirstHeight = 12;
    constexpr int kSecondWidth = 9;
    constexpr int kSecondHeight = 9;
    constexpr int kWideWidth = 18;
    constexpr int kWideHeight = 14;

    std::vector<uint8_t> const first = raster(kFirstWidth, kFirstHeight);
    std::vector<uint8_t> const second = raster(kSecondWidth, kSecondHeight);
    std::vector<uint8_t> const wide = raster(kWideWidth, kWideHeight);

    check(font.adopt('A', make_glyph(kFirstWidth, kFirstHeight, 10), first.data()),
          "the first glyph is adopted");
    expect_placed(font, 'A', kFirstWidth, kFirstHeight, first);

    check(font.adopt('e', make_glyph(kSecondWidth, kSecondHeight, 9), second.data()),
          "the second glyph is adopted");
    expect_placed(font, 'e', kSecondWidth, kSecondHeight, second);
    expect_placed(font, 'A', kFirstWidth, kFirstHeight, first);

    check(font.adopt('W', make_glyph(kWideWidth, kWideHeight, 15), wide.data()),
          "the wide glyph is adopted");
    expect_placed(font, 'W', kWideWidth, kWideHeight, wide);
    expect_placed(font, 'A', kFirstWidth, kFirstHeight, first);
    expect_placed(font, 'e', kSecondWidth, kSecondHeight, second);

    /* An advance-only glyph -- a space -- takes a place in the map and no ink. */
    check(font.adopt(' ', make_glyph(0, 0, 4), nullptr), "a blank glyph is adopted");
    Glyph const* const blank = font.glyph(' ');
    check(blank != nullptr && blank->valid && blank->advance == 4 && blank->width == 0,
          "a blank glyph is a valid advance and no box");

    std::printf("atlas: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
