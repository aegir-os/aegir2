/*
 * Trinket color types.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Premultiplied alpha RGBA, 8 bits per channel.
 * All operations assume premultiplied alpha.
 */

#ifndef AEGIR_TRINKET_COLOR_H
#define AEGIR_TRINKET_COLOR_H

#include <cstdint>
#include <algorithm>

namespace aegir::trinket {

struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;

    constexpr Color() = default;
    constexpr Color(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255)
        : r(r_), g(g_), b(b_), a(a_) {}
    constexpr Color(uint32_t rgba)  // 0xRRGGBBAA or 0x00RRGGBB
        : r((rgba >> 24) & 0xFF), g((rgba >> 16) & 0xFF),
          b((rgba >> 8) & 0xFF), a(rgba & 0xFF) {}

    // Create from non-premultiplied sRGB
    static Color from_rgb(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255) {
        if (a_ == 255) return {r_, g_, b_, a_};
        // Premultiply
        return {
            static_cast<uint8_t>((r_ * a_ + 127) / 255),
            static_cast<uint8_t>((g_ * a_ + 127) / 255),
            static_cast<uint8_t>((b_ * a_ + 127) / 255),
            a_
        };
    }

    // Convert to non-premultiplied
    constexpr uint8_t r_unpremul() const { return a == 0 ? 0 : (r * 255 + a / 2) / a; }
    constexpr uint8_t g_unpremul() const { return a == 0 ? 0 : (g * 255 + a / 2) / a; }
    constexpr uint8_t b_unpremul() const { return a == 0 ? 0 : (b * 255 + a / 2) / a; }

    // As 0xRRGGBBAA (premultiplied)
    constexpr uint32_t to_uint32() const {
        return (static_cast<uint32_t>(r) << 24) |
               (static_cast<uint32_t>(g) << 16) |
               (static_cast<uint32_t>(b) << 8) |
               static_cast<uint32_t>(a);
    }

    // As 0x00RRGGBB (for framebuffer, ignores alpha)
    constexpr uint32_t to_rgb() const {
        return (static_cast<uint32_t>(r_unpremul()) << 16) |
               (static_cast<uint32_t>(g_unpremul()) << 8) |
               static_cast<uint32_t>(b_unpremul());
    }

    constexpr bool operator==(const Color& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
    constexpr bool operator!=(const Color& other) const { return !(*this == other); }

    // Alpha blending (over operator, both premultiplied)
    static Color blend(Color src, Color dst) {
        uint8_t a = src.a + ((255 - src.a) * dst.a) / 255;
        if (a == 0) return {0, 0, 0, 0};
        return {
            static_cast<uint8_t>((src.r * 255 + dst.r * (255 - src.a)) / 255 * 255 / a + 127) / 255,
            static_cast<uint8_t>((src.g * 255 + dst.g * (255 - src.a)) / 255 * 255 / a + 127) / 255,
            static_cast<uint8_t>((src.b * 255 + dst.b * (255 - src.a)) / 255 * 255 / a + 127) / 255,
            a
        };
    }

    // Lerp between two colors (premultiplied)
    static Color lerp(Color a, Color b, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return {
            static_cast<uint8_t>(a.r + (b.r - a.r) * t + 0.5f),
            static_cast<uint8_t>(a.g + (b.g - a.g) * t + 0.5f),
            static_cast<uint8_t>(a.b + (b.b - a.b) * t + 0.5f),
            static_cast<uint8_t>(a.a + (b.a - a.a) * t + 0.5f)
        };
    }

    // Common colors (opaque, non-premultiplied for readability)
    static constexpr Color TRANSPARENT = {0, 0, 0, 0};
    static constexpr Color BLACK       = {0, 0, 0, 255};
    static constexpr Color WHITE       = {255, 255, 255, 255};
    static constexpr Color RED         = {255, 0, 0, 255};
    static constexpr Color GREEN       = {0, 255, 0, 255};
    static constexpr Color BLUE        = {0, 0, 255, 255};
    static constexpr Color YELLOW      = {255, 255, 0, 255};
    static constexpr Color CYAN        = {0, 255, 255, 255};
    static constexpr Color MAGENTA     = {255, 0, 255, 255};
    static constexpr Color GRAY        = {128, 128, 128, 255};
    static constexpr Color DARK_GRAY   = {64, 64, 64, 255};
    static constexpr Color LIGHT_GRAY  = {192, 192, 192, 255};
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_COLOR_H