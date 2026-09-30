/*
 * Trinket's theme engine: it reads the palette, the metrics and the gadget
 * recipes from the generated theme data and draws with them
 * (specs/trinket/theming.md). Nothing here is a look of its own -- the XEN look
 * is resources/themes/xen.toml -- so a theme is data and this is the one
 * interpreter that draws it.
 */

#include <aegir/trinket/theme.h>
#include <aegir/trinket/theme_data.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/unicode.h>
#include <algorithm>
#include <memory>

namespace aegir::trinket {

class DataTheme : public Theme {
public:
    explicit DataTheme(float scale = 1.0f) : scale_(scale) {}

    Color color(ColorRole role) const override {
        /* The palette is data (specs/trinket/theming.md): one colour per role,
         * in the ColorRole enum's order, from resources/themes/xen.toml. */
        return Color(kPalette[static_cast<int>(role)]);
    }

    int metric(MetricRole role) const override {
        /* The metrics are data (specs/trinket/theming.md): a base size per role
         * at 96 dpi, scaled here unless the file marks it fixed. */
        int const index = static_cast<int>(role);
        int const base = static_cast<int>(scale_ + 0.5f);
        return kMetricFixed[index] ? kMetrics[index] : kMetrics[index] * base;
    }

    Font* font() const override {
        return Application::instance()->default_font();
    }
    Font* font_small() const override { return font(); }
    Font* font_large() const override { return font(); }
    Font* font_monospace() const override { return font(); }

    /* The grey 3-D edge (specs/trinket/theme-xen.md): 1px, square, hard-edged,
     * a light top-left and a dark bottom-right for a raised gadget and the
     * reverse for a sunken one, over whatever face the caller filled. */
    void draw_bevel(Canvas& canvas, const Rect& rect, Bevel bevel) override {
        Color const light = color(ColorRole::GADGET_HIGHLIGHT);
        Color const dark = color(ColorRole::GADGET_SHADOW);
        Color const top = bevel == Bevel::RAISED ? light : dark;
        Color const bottom = bevel == Bevel::RAISED ? dark : light;
        canvas.draw_hline(rect.x, rect.x + rect.width - 1, rect.y, top);
        canvas.draw_vline(rect.y, rect.y + rect.height - 1, rect.x, top);
        canvas.draw_hline(rect.x, rect.x + rect.width - 1,
                          rect.y + rect.height - 1, bottom);
        canvas.draw_vline(rect.y, rect.y + rect.height - 1,
                          rect.x + rect.width - 1, bottom);
    }

    /* The Amiga selection: one pixel of `fg` on one of `bg`, so a selected list
     * or cycle row needs no blend (specs/trinket/theme-xen.md). */
    void draw_dither(Canvas& canvas, const Rect& rect, Color fg, Color bg) override {
        canvas.fill_rect(rect, bg);
        Rect const clip = rect.intersected(canvas.clip_rect());
        uint32_t const on = fg.to_uint32();
        for (int y = clip.y; y < clip.y + clip.height; ++y) {
            for (int x = clip.x + ((y - rect.y) & 1); x < clip.x + clip.width;
                 x += 2) {
                canvas.pixel(x, y) = on;
            }
        }
    }

    void draw_button(Canvas& canvas, const Rect& rect,
                     bool hovered, bool pressed, bool focused,
                     bool checked, bool enabled) override {
        static_cast<void>(checked);  // checked buttons are the widget's to draw
        /* The state picks the recipe (specs/trinket/theming.md): disabled beats
         * pressed beats focused beats hovered beats normal. */
        int const state = !enabled ? 4 : pressed ? 2 : focused ? 3 : hovered ? 1 : 0;
        run(kRecipeButton[state], canvas, rect);
    }

    void draw_check(Canvas& canvas, const Rect& rect, bool checked,
                    bool enabled) override {
        static_cast<void>(enabled);  // the artwork has no disabled frame
        run(kRecipeCheck[checked ? 1 : 0], canvas, rect);
    }

    void draw_radio(Canvas& canvas, const Rect& rect, bool checked,
                    bool enabled) override {
        static_cast<void>(enabled);
        run(kRecipeRadio[checked ? 1 : 0], canvas, rect);
    }

    void draw_panel(Canvas& canvas, const Rect& rect,
                     Panel::Style style, std::u32string_view title,
                     bool focused) override {
        static_cast<void>(focused);  // the focus ring is drawn separately
        run(kRecipePanel[static_cast<int>(style)], canvas, rect);
        if (style != Panel::Style::GROUP_BOX || title.empty()) return;
        Font* const font = Application::instance()->default_font();
        if (font == nullptr) return;
        Size const size = font->measure(title);
        int const gap_x = 8;
        int const x = rect.x + gap_x;
        int const y = rect.y - font->ascent() / 2;
        /* The title sits in a notch in the top edge: clear its place, then draw
         * it there. */
        canvas.fill_rect({x - 2, y, size.width + 4, font->height()},
                         color(ColorRole::PANEL_BG));
        canvas.draw_text({x, y}, title, font, color(ColorRole::TEXT));
    }

    void draw_textbox(Canvas& canvas, const Rect& rect,
                       bool focused, bool read_only, bool password) override {
        static_cast<void>(password);  // the text is the widget's to obscure
        if (read_only) {
            run(kRecipeTextboxReadonly, canvas, rect);
        } else {
            run(kRecipeTextbox[focused ? 1 : 0], canvas, rect);
        }
    }

    void draw_menubar(Canvas& canvas, const Rect& rect) override {
        canvas.fill_rect(rect, color(ColorRole::MENUBAR_BG));
        canvas.draw_hline(rect.x, rect.x + rect.width - 1, rect.y + rect.height - 1,
                          color(ColorRole::MENU_BORDER));
    }

    void draw_menu_item(Canvas& canvas, const Rect& rect,
                         const char* label, bool hovered,
                         bool checked, bool disabled, bool separator) override {
        if (separator) {
            int y = rect.y + rect.height / 2;
            canvas.draw_hline(rect.x + 20, rect.x + rect.width - 20, y, color(ColorRole::MENU_SEPARATOR));
            return;
        }

        if (hovered && !disabled) {
            canvas.fill_rect(rect, color(ColorRole::MENU_HOVER));
        }

        Font* font = Application::instance()->default_font();
        if (font && label) {
            Color text_color = disabled ? color(ColorRole::DISABLED_TEXT) : color(ColorRole::MENU_TEXT);
            int x = rect.x + metric(MetricRole::MENU_PADDING_H);
            int y = rect.y + (rect.height - font->height()) / 2;
            canvas.draw_text({x, y}, utf8_to_utf32(label),
                             font, text_color);
        }

        if (checked) {
            // Draw checkmark
            int cx = rect.x + 4;
            int cy = rect.y + rect.height / 2;
            canvas.draw_line({cx, cy}, {cx + 4, cy + 4}, color(ColorRole::MENU_TEXT));
            canvas.draw_line({cx + 4, cy + 4}, {cx + 10, cy - 4}, color(ColorRole::MENU_TEXT));
        }
    }

    void draw_titlebar(Canvas& canvas, const Rect& rect,
                        const char* title, bool active) override {
        Color bg = active ? color(ColorRole::TITLEBAR_BG) : color(ColorRole::TITLEBAR_BG_INACTIVE);
        Color text = active ? color(ColorRole::TITLEBAR_TEXT) : color(ColorRole::TITLEBAR_TEXT_INACTIVE);

        canvas.fill_rect(rect, bg);

        Font* font = Application::instance()->default_font();
        if (font && title) {
            int x = rect.x + metric(MetricRole::TITLEBAR_PADDING_H);
            int y = rect.y + (rect.height - font->height()) / 2;
            canvas.draw_text({x, y}, utf8_to_utf32(title),
                             font, text);
        }
    }

    void draw_window_frame(Canvas& canvas, const Rect& rect, bool active) override {
        static_cast<void>(active);  // the Workbench frame is the same either way
        Color const light = color(ColorRole::FRAME_LIGHT);
        Color const dark = color(ColorRole::FRAME_DARK);
        canvas.draw_hline(rect.x, rect.x + rect.width - 1, rect.y, light);
        canvas.draw_vline(rect.y, rect.y + rect.height - 1, rect.x, light);
        canvas.draw_hline(rect.x, rect.x + rect.width - 1,
                          rect.y + rect.height - 1, dark);
        canvas.draw_vline(rect.y, rect.y + rect.height - 1,
                          rect.x + rect.width - 1, dark);
    }

    void draw_scrollbar(Canvas& canvas, const Rect& rect, bool vertical,
                         const Rect& trough, const Rect& thumb,
                         const Rect& decrement, const Rect& increment,
                         bool hovered) override {
        static_cast<void>(vertical);
        static_cast<void>(hovered);
        static_cast<void>(rect);  // the trough and the buttons are the whole strip
        /* Each part is a recipe (specs/trinket/theming.md); the buttons are
         * separate cells below the trough, their marks in their own recipes. */
        if (trough.width > 0 && trough.height > 0) {
            run(kRecipeScrollbarTrough, canvas, trough);
        }
        if (thumb.width > 0 && thumb.height > 0) {
            run(kRecipeScrollbarThumb, canvas, thumb);
        }
        if (decrement.width > 0 && decrement.height > 0) {
            run(kRecipeScrollbarDecrement, canvas, decrement);
        }
        if (increment.width > 0 && increment.height > 0) {
            run(kRecipeScrollbarIncrement, canvas, increment);
        }
    }

    /* Focus is the gadget's own active state (specs/trinket/theme-xen.md): the
     * button and the field draw it, so there is no separate ring. */
    void draw_focus_ring(Canvas&, const Rect&) override {}

    void draw_tooltip(Canvas& canvas, const Rect& rect,
                       const char* text) override {
        canvas.fill_rounded_rect(rect, 4, color(ColorRole::TOOLTIP_BG));
        Font* font = Application::instance()->default_font();
        if (font && text) {
            int x = rect.x + 6;
            int y = rect.y + (rect.height - font->height()) / 2;
            canvas.draw_text({x, y}, utf8_to_utf32(text),
                             font, color(ColorRole::TOOLTIP_TEXT));
        }
    }

private:
    /* Run a gadget's recipe (specs/trinket/theming.md): each step draws one
     * primitive into the gadget's rectangle, set in by its inset. */
    void run(Recipe const& recipe, Canvas& canvas, Rect const& rect) {
        for (int i = 0; i < recipe.count; ++i) {
            Step const& step = recipe.steps[i];
            Rect const r = rect.inflated(-step.inset);
            switch (step.op) {
                case Prim::FILL:
                    canvas.fill_rect(r, Color(step.color));
                    break;
                case Prim::BEVEL:
                    draw_bevel(canvas, r, step.kind ? Bevel::SUNKEN : Bevel::RAISED);
                    break;
                case Prim::OUTLINE:
                    canvas.draw_rect(r, Color(step.color));
                    break;
                case Prim::DITHER:
                    draw_dither(canvas, r, Color(step.color), Color(step.color2));
                    break;
                case Prim::MARK:
                    draw_mark(canvas, r, step.kind, Color(step.color), Color(step.color2),
                              step.num, step.den);
                    break;
                case Prim::SPRITE:
                    blit_sprite(canvas, r, step.sprite);
                    break;
            }
        }
    }

    /* A hollow 3-D triangle: the leading (left) edge light and the other two
     * edges dark, inset from the rectangle -- the mark the XEN art shows
     * (specs/trinket/scrollbar.md). */
    void draw_mark(Canvas& canvas, const Rect& rect, int kind, Color light,
                   Color dark, int num, int den) {
        int const short_side = std::min(rect.width, rect.height);
        if (short_side <= 0 || den <= 0) return;
        int const half = std::max(1, short_side * num / den);
        Point const c = rect.center();
        bool const vertical = kind <= 1;
        bool const forward = kind == 1 || kind == 3;  // down or right
        if (vertical) {
            int const base = forward ? c.y - half : c.y + half;
            int const apex = forward ? c.y + half : c.y - half;
            Point const apex_pt{c.x, apex};
            Point const left{c.x - half, base};
            Point const right{c.x + half, base};
            canvas.draw_line(left, apex_pt, light);
            canvas.draw_line(apex_pt, right, dark);
            canvas.draw_line(left, right, dark);
        } else {
            int const base = forward ? c.x - half : c.x + half;
            int const apex = forward ? c.x + half : c.x - half;
            Point const apex_pt{apex, c.y};
            Point const top{base, c.y - half};
            Point const bottom{base, c.y + half};
            canvas.draw_line(top, apex_pt, light);
            canvas.draw_line(apex_pt, bottom, dark);
            canvas.draw_line(top, bottom, dark);
        }
    }

    /* Blit a sprite into the rectangle, scaled to it (nearest neighbour) --
     * the art is at one point size and the bar another, so the buttons keep
     * their proportion -- skipping transparent pixels and clipping to the
     * rectangle and the canvas (specs/trinket/theming.md). */
    void blit_sprite(Canvas& canvas, const Rect& rect, int index) {
        if (index < 0 || index >= static_cast<int>(kSpriteCount)) return;
        Sprite const& sprite = kSprites[index];
        if (sprite.width == 0 || sprite.height == 0) return;
        Rect const clip = rect.intersected(canvas.clip_rect());
        if (clip.empty()) return;
        for (int py = clip.y; py < clip.y + clip.height; ++py) {
            int const sy = (py - rect.y) * sprite.height / rect.height;
            for (int px = clip.x; px < clip.x + clip.width; ++px) {
                int const sx = (px - rect.x) * sprite.width / rect.width;
                uint32_t const pixel = sprite.pixels[sy * sprite.width + sx];
                if ((pixel >> 24) == 0) continue;
                canvas.pixel(px, py) = pixel & 0xFFFFFFu;
            }
        }
    }

    float scale_ = 1.0f;
};

std::unique_ptr<Theme> Theme::create_xen(float scale) {
    return std::make_unique<DataTheme>(scale);
}

std::unique_ptr<Theme> Theme::create_high_contrast(float scale) {
    // TODO: High contrast theme
    return create_xen(scale);
}

} // namespace aegir::trinket