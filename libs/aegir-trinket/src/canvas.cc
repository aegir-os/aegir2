/*
 * Trinket Canvas implementation.
 */

#include <aegir/trinket/canvas.h>
#include <aegir/trinket/font.h>
#include <algorithm>

namespace aegir::trinket {

Canvas::Canvas() = default;

Canvas::Canvas(uint32_t* pixels, int width, int height, int stride)
    : pixels_(pixels), width_(width), height_(height), stride_(stride) {
    clip_rect_ = rect();
}

void Canvas::clear(Color c) {
    uint32_t val = c.to_uint32();
    for (int y = 0; y < height_; ++y) {
        uint32_t* row = pixels_ + y * stride_;
        for (int x = 0; x < width_; ++x) {
            row[x] = val;
        }
    }
}

void Canvas::clear_rect(const Rect& r, Color c) {
    Rect clipped = r.intersected(clip_rect_);
    if (clipped.empty()) return;
    uint32_t val = c.to_uint32();
    for (int y = clipped.y; y < clipped.y + clipped.height; ++y) {
        uint32_t* row = pixels_ + y * stride_;
        for (int x = clipped.x; x < clipped.x + clipped.width; ++x) {
            row[x] = val;
        }
    }
}

void Canvas::fill_rect(const Rect& r, Color c) {
    Rect clipped = r.intersected(clip_rect_);
    if (clipped.empty()) return;
    if (c.a == 255) {
        uint32_t val = c.to_uint32();
        for (int y = clipped.y; y < clipped.y + clipped.height; ++y) {
            uint32_t* row = pixels_ + y * stride_;
            for (int x = clipped.x; x < clipped.x + clipped.width; ++x) {
                row[x] = val;
            }
        }
    } else {
        for (int y = clipped.y; y < clipped.y + clipped.height; ++y) {
            for (int x = clipped.x; x < clipped.x + clipped.width; ++x) {
                pixels_[y * stride_ + x] = Color::blend(c, Color(pixels_[y * stride_ + x])).to_uint32();
            }
        }
    }
}

void Canvas::fill_rect(int x, int y, int w, int h, Color c) {
    fill_rect(Rect{x, y, w, h}, c);
}

void Canvas::draw_rect(const Rect& r, Color c, int thickness) {
    if (thickness <= 0) return;
    Rect clipped = r.intersected(clip_rect_);
    if (clipped.empty()) return;
    uint32_t val = c.to_uint32();
    for (int t = 0; t < thickness; ++t) {
        // Top
        for (int x = r.x; x < r.x + r.width; ++x) {
            if (r.y + t >= clip_rect_.y && r.y + t < clip_rect_.y + clip_rect_.height &&
                x >= clip_rect_.x && x < clip_rect_.x + clip_rect_.width) {
                pixels_[(r.y + t) * stride_ + x] = val;
            }
        }
        // Bottom
        for (int x = r.x; x < r.x + r.width; ++x) {
            if (r.y + r.height - 1 - t >= clip_rect_.y && r.y + r.height - 1 - t < clip_rect_.y + clip_rect_.height &&
                x >= clip_rect_.x && x < clip_rect_.x + clip_rect_.width) {
                pixels_[(r.y + r.height - 1 - t) * stride_ + x] = val;
            }
        }
        // Left
        for (int y = r.y; y < r.y + r.height; ++y) {
            if (r.x + t >= clip_rect_.x && r.x + t < clip_rect_.x + clip_rect_.width &&
                y >= clip_rect_.y && y < clip_rect_.y + clip_rect_.height) {
                pixels_[y * stride_ + r.x + t] = val;
            }
        }
        // Right
        for (int y = r.y; y < r.y + r.height; ++y) {
            if (r.x + r.width - 1 - t >= clip_rect_.x && r.x + r.width - 1 - t < clip_rect_.x + clip_rect_.width &&
                y >= clip_rect_.y && y < clip_rect_.y + clip_rect_.height) {
                pixels_[y * stride_ + r.x + r.width - 1 - t] = val;
            }
        }
    }
}

void Canvas::draw_line(Point a, Point b, Color c, int thickness) {
    // Bresenham's line algorithm
    int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    uint32_t val = c.to_uint32();

    while (true) {
        if (x0 >= clip_rect_.x && x0 < clip_rect_.x + clip_rect_.width &&
            y0 >= clip_rect_.y && y0 < clip_rect_.y + clip_rect_.height) {
            pixels_[y0 * stride_ + x0] = val;
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Canvas::draw_hline(int x1, int x2, int y, Color c, int thickness) {
    if (x1 > x2) std::swap(x1, x2);
    for (int t = 0; t < thickness; ++t) {
        int ty = y + t;
        if (ty < clip_rect_.y || ty >= clip_rect_.y + clip_rect_.height) continue;
        for (int x = x1; x <= x2; ++x) {
            if (x >= clip_rect_.x && x < clip_rect_.x + clip_rect_.width) {
                pixels_[ty * stride_ + x] = c.to_uint32();
            }
        }
    }
}

void Canvas::draw_vline(int y1, int y2, int x, Color c, int thickness) {
    if (y1 > y2) std::swap(y1, y2);
    for (int t = 0; t < thickness; ++t) {
        int tx = x + t;
        if (tx < clip_rect_.x || tx >= clip_rect_.x + clip_rect_.width) continue;
        for (int y = y1; y <= y2; ++y) {
            if (y >= clip_rect_.y && y < clip_rect_.y + clip_rect_.height) {
                pixels_[y * stride_ + tx] = c.to_uint32();
            }
        }
    }
}

void Canvas::fill_rounded_rect(const Rect& r, int radius, Color c) {
    radius = std::min(radius, std::min(r.width, r.height) / 2);
    if (radius <= 0) {
        fill_rect(r, c);
        return;
    }

    // Fill center rectangle
    Rect center = {r.x + radius, r.y, r.width - 2 * radius, r.height};
    fill_rect(center, c);

    // Fill top and bottom strips
    Rect top = {r.x + radius, r.y, r.width - 2 * radius, radius};
    Rect bottom = {r.x + radius, r.y + r.height - radius, r.width - 2 * radius, radius};
    fill_rect(top, c);
    fill_rect(bottom, c);

    // Fill corners (approximate with quarter circles)
    for (int y = 0; y < radius; ++y) {
        int x = static_cast<int>(std::sqrt(radius * radius - (radius - y) * (radius - y)));
        // Top-left
        draw_hline(r.x, r.x + x, r.y + y, c);
        draw_hline(r.x + r.width - 1 - x, r.x + r.width - 1, r.y + y, c);
        // Bottom-left
        draw_hline(r.x, r.x + x, r.y + r.height - 1 - y, c);
        draw_hline(r.x + r.width - 1 - x, r.x + r.width - 1, r.y + r.height - 1 - y, c);
    }
}

void Canvas::draw_rounded_rect(const Rect& r, int radius, Color c, int thickness) {
    radius = std::min(radius, std::min(r.width, r.height) / 2);
    if (radius <= 0) {
        draw_rect(r, c, thickness);
        return;
    }
    // Simplified: draw straight edges and corner arcs
    for (int t = 0; t < thickness; ++t) {
        // Top edge
        draw_hline(r.x + radius, r.x + r.width - radius - 1, r.y + t, c);
        // Bottom edge
        draw_hline(r.x + radius, r.x + r.width - radius - 1, r.y + r.height - 1 - t, c);
        // Left edge
        draw_vline(r.y + radius, r.y + r.height - radius - 1, r.x + t, c);
        // Right edge
        draw_vline(r.y + radius, r.y + r.height - radius - 1, r.x + r.width - 1 - t, c);
    }
    // Corners (approximate)
    for (int i = 0; i < radius; ++i) {
        int x = static_cast<int>(std::sqrt(radius * radius - i * i));
        // Top-left
        draw_hline(r.x + radius - x, r.x + radius, r.y + radius - i, c);
        draw_vline(r.y + radius - i, r.y + radius, r.x + radius - x, c);
        // Top-right
        draw_hline(r.x + r.width - radius, r.x + r.width - radius + x, r.y + radius - i, c);
        draw_vline(r.y + radius - i, r.y + radius, r.x + r.width - radius + x, c);
        // Bottom-left
        draw_hline(r.x + radius - x, r.x + radius, r.y + r.height - radius + i, c);
        draw_vline(r.y + r.height - radius + i, r.y + r.height - radius, r.x + radius - x, c);
        // Bottom-right
        draw_hline(r.x + r.width - radius, r.x + r.width - radius + x, r.y + r.height - radius + i, c);
        draw_vline(r.y + r.height - radius + i, r.y + r.height - radius, r.x + r.width - radius + x, c);
    }
}

void Canvas::fill_circle(Point center, int radius, Color c) {
    for (int y = -radius; y <= radius; ++y) {
        int x = static_cast<int>(std::sqrt(radius * radius - y * y));
        draw_hline(center.x - x, center.x + x, center.y + y, c);
    }
}

void Canvas::draw_circle(Point center, int radius, Color c, int thickness) {
    // Midpoint circle algorithm
    int x = radius, y = 0;
    int err = 0;
    uint32_t val = c.to_uint32();

    while (x >= y) {
        for (int t = 0; t < thickness; ++t) {
            // 8 octants
            auto draw = [&](int px, int py) {
                if (px >= clip_rect_.x && px < clip_rect_.x + clip_rect_.width &&
                    py >= clip_rect_.y && py < clip_rect_.y + clip_rect_.height) {
                    pixels_[py * stride_ + px] = val;
                }
            };
            draw(center.x + x, center.y + y);
            draw(center.x + y, center.y + x);
            draw(center.x - y, center.y + x);
            draw(center.x - x, center.y + y);
            draw(center.x - x, center.y - y);
            draw(center.x - y, center.y - x);
            draw(center.x + y, center.y - x);
            draw(center.x + x, center.y - y);
        }
        y += 1;
        err += 1 + 2 * y;
        if (2 * (err - x) + 1 > 0) {
            x -= 1;
            err += 1 - 2 * x;
        }
    }
}

void Canvas::draw_text(Point pos, std::u32string_view text, Font* font, Color color,
                       BidiDirection dir) {
    if (!font || text.empty()) return;
    int x = pos.x;
    int y = pos.y + font->ascent();

    for (char32_t cp : text) {
        const Glyph* g = font->find_glyph(cp);
        if (!g || !g->valid) continue;

        // Draw glyph from atlas
        const auto& atlas = static_cast<const BitmapFont*>(font)->atlas();
        if (atlas.pixels.empty()) continue;

        int gw = g->width;
        int gh = g->height;
        if (gw <= 0 || gh <= 0) continue;

        int sx = g->atlas_x;
        int sy = g->atlas_y;

        for (int gy = 0; gy < gh; ++gy) {
            int ty = y + gy - g->bearing_y;
            if (ty < clip_rect_.y || ty >= clip_rect_.y + clip_rect_.height) continue;
            int sy_atlas = sy + gy;
            if (sy_atlas >= static_cast<int>(atlas.height)) continue;

            for (int gx = 0; gx < gw; ++gx) {
                int tx = x + gx + g->bearing_x;
                if (tx < clip_rect_.x || tx >= clip_rect_.x + clip_rect_.width) continue;
                int sx_atlas = sx + gx;
                if (sx_atlas >= static_cast<int>(atlas.width)) continue;

                uint8_t alpha = atlas.pixels[sy_atlas * atlas.width + sx_atlas];
                if (alpha == 0) continue;

                Color pixel_color = color;
                pixel_color.a = alpha;
                pixels_[ty * stride_ + tx] = Color::blend(pixel_color,
                    Color(pixels_[ty * stride_ + tx])).to_uint32();
            }
        }
        x += g->advance;
    }
}

void Canvas::draw_text(Point pos, std::string_view utf8, Font* font, Color color,
                       BidiDirection dir) {
    draw_text(pos, utf8_to_utf32(utf8), font, color, dir);
}

void Canvas::draw_text_clipped(const Rect& clip, Point pos, std::u32string_view text,
                               Font* font, Color color, BidiDirection dir) {
    Rect old_clip = clip_rect_;
    set_clip_rect(clip);
    draw_text(pos, text, font, color, dir);
    set_clip_rect(old_clip);
}

Size Canvas::measure_text(std::u32string_view text, Font* font) const {
    if (!font) return {0, 0};
    int width = 0;
    for (char32_t cp : text) {
        const Glyph* g = font->glyph(cp);
        if (g) width += g->advance;
    }
    return {width, font->height()};
}

Size Canvas::measure_text(std::string_view utf8, Font* font) const {
    return measure_text(utf8_to_utf32(utf8), font);
}

void Canvas::draw_bitmap(const Rect& dest, const uint8_t* src_pixels, int src_w, int src_h,
                         int src_stride, bool has_alpha) {
    Rect clipped = dest.intersected(clip_rect_);
    if (clipped.empty()) return;

    int src_x = clipped.x - dest.x;
    int src_y = clipped.y - dest.y;
    int w = clipped.width;
    int h = clipped.height;

    for (int y = 0; y < h; ++y) {
        int ty = clipped.y + y;
        int sy = src_y + y;
        for (int x = 0; x < w; ++x) {
            int tx = clipped.x + x;
            int sx = src_x + x;
            uint8_t alpha = has_alpha ? src_pixels[sy * src_stride + sx * 4 + 3] : 255;
            if (alpha == 0) continue;

            uint8_t r = src_pixels[sy * src_stride + sx * 4];
            uint8_t g = src_pixels[sy * src_stride + sx * 4 + 1];
            uint8_t b = src_pixels[sy * src_stride + sx * 4 + 2];

            Color src_color = Color::from_rgb(r, g, b, alpha);
            Color dst_color(pixels_[ty * stride_ + tx]);
            pixels_[ty * stride_ + tx] = Color::blend(src_color, dst_color).to_uint32();
        }
    }
}

void Canvas::add_damage(const Rect& r) {
    damage_rects_.push_back(r);
}

void Canvas::clear_damage() {
    damage_rects_.clear();
}

} // namespace aegir::trinket