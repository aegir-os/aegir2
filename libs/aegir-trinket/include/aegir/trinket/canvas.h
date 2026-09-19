/*
 * Trinket Canvas - drawing surface.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_CANVAS_H
#define AEGIR_TRINKET_CANVAS_H

#include <aegir/trinket/point.h>
#include <aegir/trinket/color.h>
#include <aegir/trinket/font.h>
#include <cstdint>
#include <string>

namespace aegir::trinket {

class Canvas {
public:
    Canvas() = default;
    Canvas(uint32_t* pixels, int width, int height, int stride);
    ~Canvas() = default;

    // Non-copyable, movable
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    Canvas(Canvas&&) noexcept = default;
    Canvas& operator=(Canvas&&) noexcept = default;

    // Geometry
    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return stride_; }
    Rect rect() const { return {0, 0, width_, height_}; }

    // Pixel access
    uint32_t* pixels() const { return pixels_; }
    uint32_t& pixel(int x, int y) { return pixels_[y * stride_ + x]; }
    const uint32_t& pixel(int x, int y) const { return pixels_[y * stride_ + x]; }

    // Clear
    void clear(Color c = Color::TRANSPARENT);
    void clear_rect(const Rect& r, Color c);

    // Basic shapes
    void fill_rect(const Rect& r, Color c);
    void fill_rect(int x, int y, int w, int h, Color c);
    void draw_rect(const Rect& r, Color c, int thickness = 1);
    void draw_line(Point a, Point b, Color c, int thickness = 1);
    void draw_hline(int x1, int x2, int y, Color c, int thickness = 1);
    void draw_vline(int y1, int y2, int x, Color c, int thickness = 1);

    // Rounded rect
    void fill_rounded_rect(const Rect& r, int radius, Color c);
    void draw_rounded_rect(const Rect& r, int radius, Color c, int thickness = 1);

    // Circle/ellipse
    void fill_circle(Point center, int radius, Color c);
    void draw_circle(Point center, int radius, Color c, int thickness = 1);

    // Text rendering (simple, no shaping)
    void draw_text(Point pos, std::u32string_view text, Font* font, Color color,
                   BidiDirection dir = BidiDirection::LTR);
    void draw_text(Point pos, std::string_view utf8, Font* font, Color color,
                   BidiDirection dir = BidiDirection::LTR);

    // Text with clipping
    void draw_text_clipped(const Rect& clip, Point pos, std::u32string_view text,
                           Font* font, Color color, BidiDirection dir = BidiDirection::LTR);

    // Text measurement
    Size measure_text(std::u32string_view text, Font* font) const;
    Size measure_text(std::string_view utf8, Font* font) const;

    // Image/bitmap
    void draw_bitmap(const Rect& dest, const uint8_t* src_pixels, int src_w, int src_h,
                     int src_stride, bool has_alpha = true);

    // Alpha blending
    void set_blend_mode(bool enabled) { blend_mode_ = enabled; }
    bool blend_mode() const { return blend_mode_; }

    // Clip rect
    void set_clip_rect(const Rect& r) { clip_rect_ = r; }
    void clear_clip_rect() { clip_rect_ = rect(); }
    Rect clip_rect() const { return clip_rect_; }

    // Damage tracking (for partial repaint)
    void add_damage(const Rect& r);
    void clear_damage();
    const std::vector<Rect>& damage_rects() const { return damage_rects_; }

private:
    uint32_t* pixels_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    bool blend_mode_ = true;
    Rect clip_rect_;
    std::vector<Rect> damage_rects_;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_CANVAS_H