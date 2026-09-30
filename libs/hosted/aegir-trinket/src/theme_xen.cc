/*
 * Trinket XEN/Workbench theme implementation.
 */

#include <aegir/trinket/theme.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/unicode.h>
#include <algorithm>
#include <memory>

namespace aegir::trinket {

class XENTheme : public Theme {
public:
    explicit XENTheme(float scale = 1.0f) : scale_(scale) {}

    Color color(ColorRole role) const override {
        using CR = ColorRole;
        // XEN/Workbench color palette (scaled for 96 DPI base)
        switch (role) {
            // Base
            case CR::BACKGROUND: return Color(0x00AAAAAA);       // Workbench grey
            case CR::WINDOW_BG: return Color(0x00AAAAAA);        // the body
            case CR::PANEL_BG: return Color(0x00AAAAAA);         // a group's face

            // Text
            case CR::TEXT: return Color(0x00000000);             // Black
            case CR::TEXT_DISABLED: return Color(0x00808080);    // Grey
            case CR::TEXT_SELECTED: return Color(0x00FFFFFF);    // White
            case CR::TEXT_INVERSE: return Color(0x00FFFFFF);

            // Buttons (specs/trinket/theme-xen.md): a raised #bfbfbf face whose
            // edge is the 3-D bevel, and focus is the gadget's active state.
            case CR::BUTTON_BG: return Color(0x00BFBFBF);
            case CR::BUTTON_HOVER: return Color(0x00C9C9C9);
            case CR::BUTTON_PRESSED: return Color(0x00BFBFBF);
            case CR::BUTTON_FOCUS: return Color(0x00000000);
            case CR::BUTTON_TEXT: return Color(0x00000000);
            case CR::BUTTON_BORDER: return Color(0x00000000);

            // Input: a sunken #bfbfbf well.
            case CR::INPUT_BG: return Color(0x00BFBFBF);
            case CR::INPUT_TEXT: return Color(0x00000000);
            case CR::INPUT_PLACEHOLDER: return Color(0x00808080);
            case CR::INPUT_BORDER: return Color(0x008C8C8C);
            case CR::INPUT_FOCUS_BORDER: return Color(0x00000000);

            // Selection: the Amiga dithered blue (XEN chrome blue).
            case CR::SELECTION_BG: return Color(0x006688BB);
            case CR::SELECTION_TEXT: return Color(0x00FFFFFF);

            // Accent
            case CR::ACCENT: return Color(0x006688BB);
            case CR::ACCENT_HOVER: return Color(0x007799CC);
            case CR::ACCENT_PRESSED: return Color(0x00446699);

            // Borders
            case CR::BORDER: return Color(0x00000000);
            case CR::BORDER_LIGHT: return Color(0x00FFFFFF);
            case CR::BORDER_DARK: return Color(0x00000000);
            case CR::FOCUS_BORDER: return Color(0x00000000);

            // Menu
            case CR::MENUBAR_BG: return Color(0x00E0E0E0);
            case CR::MENUBAR_HOVER: return Color(0x000078D7);
            case CR::MENUBAR_TEXT: return Color(0x00000000);
            case CR::MENU_BG: return Color(0x00F0F0F0);
            case CR::MENU_HOVER: return Color(0x000078D7);
            case CR::MENU_TEXT: return Color(0x00000000);
            case CR::MENU_BORDER: return Color(0x00808080);
            case CR::MENU_SEPARATOR: return Color(0x00808080);

            // Titlebar (Workbench XEN, specs/amiga-fidelity.md): the same
            // fill active or not -- the active/inactive difference is the
            // gadgets' fill (window.cc) -- and the title is black either way.
            case CR::TITLEBAR_BG: return Color(0x006688BB);
            case CR::TITLEBAR_BG_INACTIVE: return Color(0x006688BB);
            case CR::TITLEBAR_TEXT: return Color(0x00000000);
            case CR::TITLEBAR_TEXT_INACTIVE: return Color(0x00000000);
            case CR::TITLEBAR_BUTTON_BG: return Color(0x006688BB);
            case CR::TITLEBAR_BUTTON_HOVER: return Color(0x40FFFFFF);
            case CR::TITLEBAR_HIGHLIGHT: return Color(0x00A4B8D7);
            case CR::TITLEBAR_SHADOW: return Color(0x0043597B);
            case CR::BOTTOMBAR_HIGHLIGHT: return Color(0x00B1C2DC);
            case CR::BOTTOMBAR_SHADOW: return Color(0x002A384D);
            case CR::GADGET_OUTLINE: return Color(0x00030303);
            case CR::GADGET_GREY: return Color(0x00AAAAAA);
            case CR::GADGET_WHITE: return Color(0x00FFFFFF);
            case CR::FRAME_LIGHT: return Color(0x00F7F7F7);
            case CR::FRAME_DARK: return Color(0x002A384D);

            // Grey 3-D gadgets (specs/trinket/theme-xen.md): a raised face and
            // the two edges its bevel is drawn from, plus the soft second
            // shadow some frames carry. Read off the MUI XEN screenshots.
            case CR::GADGET_FACE: return Color(0x00BFBFBF);
            case CR::GADGET_HIGHLIGHT: return Color(0x00FFFFFF);
            case CR::GADGET_SHADOW: return Color(0x00000000);
            case CR::GADGET_SOFT_SHADOW: return Color(0x008C8C8C);

            // Tooltip
            case CR::TOOLTIP_BG: return Color(0xE0FFFF80);       // Yellow
            case CR::TOOLTIP_TEXT: return Color(0x00000000);

            // Scrollbar
            case CR::SCROLLBAR_BG: return Color(0x00AAAAAA);
            case CR::SCROLLBAR_HANDLE: return Color(0x00BFBFBF);
            case CR::SCROLLBAR_HANDLE_HOVER: return Color(0x00C9C9C9);

            // Link
            case CR::LINK: return Color(0x000000EE);
            case CR::LINK_VISITED: return Color(0x00800080);
            case CR::LINK_HOVER: return Color(0x00FF0000);

            // Status
            case CR::ERROR: return Color(0x00CC0000);
            case CR::WARNING: return Color(0x00CC8800);
            case CR::SUCCESS: return Color(0x00008800);
            case CR::INFO: return Color(0x000078D7);

            // Disabled
            case CR::DISABLED_BG: return Color(0x00E0E0E0);
            case CR::DISABLED_TEXT: return Color(0x00808080);
        }
        return Color::TRANSPARENT;
    }

    int metric(MetricRole role) const override {
        using MR = MetricRole;
        int base = static_cast<int>(scale_ + 0.5f);
        switch (role) {
            case MR::BUTTON_PADDING_H: return 12 * base;
            case MR::BUTTON_PADDING_V: return 6 * base;
            case MR::BUTTON_MIN_WIDTH: return 72 * base;
            case MR::BUTTON_MIN_HEIGHT: return 24 * base;
            case MR::BUTTON_BORDER_WIDTH: return 1 * base;
            case MR::BUTTON_RADIUS: return 2 * base;
            case MR::PANEL_BORDER_WIDTH: return 1 * base;
            case MR::PANEL_RADIUS: return 0;
            case MR::MENUBAR_HEIGHT: return 22 * base;
            case MR::MENU_ITEM_HEIGHT: return 22 * base;
            case MR::MENU_PADDING_H: return 8 * base;
            case MR::MENU_PADDING_V: return 3 * base;
            case MR::MENU_SEPARATOR_HEIGHT: return 1 * base;
            case MR::MENU_BORDER_WIDTH: return 1 * base;
            case MR::TITLEBAR_HEIGHT: return 24 * base;
            case MR::TITLEBAR_BUTTON_SIZE: return 16 * base;
            case MR::TITLEBAR_PADDING_H: return 8 * base;
            case MR::WINDOW_BORDER_WIDTH: return 1 * base;
            case MR::WINDOW_SHADOW_WIDTH: return 0;
            case MR::INPUT_PADDING_H: return 8 * base;
            case MR::INPUT_PADDING_V: return 4 * base;
            case MR::INPUT_BORDER_WIDTH: return 1 * base;
            case MR::SCROLLBAR_WIDTH: return 16 * base;
            case MR::SCROLLBAR_MIN_HANDLE: return 30 * base;
            case MR::SCROLLBAR_ARROW_SIZE: return 16 * base;
            case MR::SPACING_SMALL: return 4 * base;
            case MR::SPACING_MEDIUM: return 8 * base;
            case MR::SPACING_LARGE: return 16 * base;
            case MR::FOCUS_RING_WIDTH: return 2 * base;
            case MR::FOCUS_RING_OFFSET: return 2 * base;
            case MR::ICON_SIZE_SMALL: return 16 * base;
            case MR::ICON_SIZE_NORMAL: return 24 * base;
            case MR::ICON_SIZE_LARGE: return 32 * base;
            case MR::TOOLTIP_DELAY_MS: return 500;
            case MR::TEXT_LINE_HEIGHT_MULTIPLIER: return 120;  // 1.2x
        }
        return 0;
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
        Color const face = !enabled ? color(ColorRole::DISABLED_BG)
                           : pressed ? color(ColorRole::BUTTON_PRESSED)
                           : hovered ? color(ColorRole::BUTTON_HOVER)
                                     : color(ColorRole::BUTTON_BG);
        canvas.fill_rect(rect, face);
        /* Focus is the gadget's active state (specs/trinket/theme-xen.md): a
         * pressed or focused gadget is inset, a raised one is not. */
        draw_bevel(canvas, rect,
                   (pressed || focused) ? Bevel::SUNKEN : Bevel::RAISED);
    }

    void draw_panel(Canvas& canvas, const Rect& rect,
                     Panel::Style style, std::u32string_view title,
                     bool focused) override {
        static_cast<void>(focused);  // the focus ring is drawn separately
        Color const bg = color(ColorRole::PANEL_BG);
        switch (style) {
            case Panel::Style::FLAT:
                canvas.fill_rect(rect, bg);
                break;
            case Panel::Style::RAISED:
                canvas.fill_rect(rect, bg);
                draw_bevel(canvas, rect, Bevel::RAISED);
                break;
            case Panel::Style::SUNKEN:
                canvas.fill_rect(rect, bg);
                draw_bevel(canvas, rect, Bevel::SUNKEN);
                break;
            case Panel::Style::FRAME:
                canvas.fill_rect(rect, bg);
                canvas.draw_rect(rect, color(ColorRole::BORDER),
                                 metric(MetricRole::PANEL_BORDER_WIDTH));
                break;
            case Panel::Style::GROUP_BOX: {
                canvas.fill_rect(rect, bg);
                draw_bevel(canvas, rect, Bevel::RAISED);
                if (title.empty()) break;
                Font* const font = Application::instance()->default_font();
                if (font == nullptr) break;
                Size const size = font->measure(title);
                int const gap_x = 8;
                int const x = rect.x + gap_x;
                int const y = rect.y - font->ascent() / 2;
                /* The title sits in a notch in the top edge: clear its place,
                 * then draw it there. */
                canvas.fill_rect({x - 2, y, size.width + 4, font->height()}, bg);
                canvas.draw_text({x, y}, title, font, color(ColorRole::TEXT));
                break;
            }
        }
    }

    void draw_textbox(Canvas& canvas, const Rect& rect,
                       bool focused, bool read_only, bool password) override {
        static_cast<void>(password);  // the text is the widget's to obscure
        Color const bg = read_only ? color(ColorRole::DISABLED_BG)
                                   : color(ColorRole::INPUT_BG);
        canvas.fill_rect(rect, bg);
        draw_bevel(canvas, rect, Bevel::SUNKEN);
        if (focused) {
            /* The active state (specs/trinket/theme-xen.md): a full black
             * outline inside the well, in place of the old blue ring. */
            canvas.draw_rect(rect.inflated(-1),
                             color(ColorRole::INPUT_FOCUS_BORDER));
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
        static_cast<void>(hovered);
        Color const face = color(ColorRole::GADGET_FACE);
        Color const frame_face = color(ColorRole::PANEL_BG);
        Color const dither = color(ColorRole::SELECTION_BG);
        Color const trough_bg = color(ColorRole::SCROLLBAR_BG);
        Color const thumb_face = color(ColorRole::SCROLLBAR_HANDLE);
        Color const ink = color(ColorRole::GADGET_SHADOW);

        /* The scrollbar's own raised frame, around the whole strip: a black
         * outline, then the light top-left bevel *inside* it, over the grey
         * face -- drawing the outline last would flatten it. */
        canvas.fill_rect(rect, frame_face);
        canvas.draw_rect(rect, ink);
        draw_bevel(canvas, rect.inflated(-1), Bevel::RAISED);

        /* The trough: the MUI XEN dither in a sunken well. */
        if (trough.width > 0 && trough.height > 0) {
            draw_dither(canvas, trough, dither, trough_bg);
            draw_bevel(canvas, trough, Bevel::SUNKEN);
        }

        /* The thumb, a raised block narrower than the bar, in the well. */
        if (thumb.width > 0 && thumb.height > 0) {
            canvas.fill_rect(thumb, thumb_face);
            draw_bevel(canvas, thumb, Bevel::RAISED);
        }

        /* The two raised buttons at the foot, each with a hollow 3-D mark. */
        Rect const buttons[2] = {decrement, increment};
        for (Rect const& button : buttons) {
            if (button.width <= 0 || button.height <= 0) continue;
            canvas.fill_rect(button, face);
            canvas.draw_rect(button, ink);
            draw_bevel(canvas, button.inflated(-1), Bevel::RAISED);
        }
        draw_arrow(canvas, decrement, vertical, false, ink);
        draw_arrow(canvas, increment, vertical, true, ink);
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
    /* A hollow 3-D triangle: the leading (left) edge light and the other two
     * edges dark, so it reads as the XEN arrow rather than a solid mark
     * (specs/trinket/scrollbar.md). */
    void draw_arrow(Canvas& canvas, const Rect& button, bool vertical,
                    bool forward, Color dark) {
        int const size = std::min(button.width, button.height) * 3 / 5;
        if (size < 1) return;
        Color const light = color(ColorRole::GADGET_HIGHLIGHT);
        Point const c = button.center();
        if (vertical) {
            int const base = forward ? c.y - size / 2 : c.y + size / 2;
            int const apex = forward ? c.y + size / 2 : c.y - size / 2;
            Point const apex_pt{c.x, apex};
            Point const left{c.x - size, base};
            Point const right{c.x + size, base};
            canvas.draw_line(left, apex_pt, light);
            canvas.draw_line(apex_pt, right, dark);
            canvas.draw_line(left, right, dark);
        } else {
            int const base = forward ? c.x - size / 2 : c.x + size / 2;
            int const apex = forward ? c.x + size / 2 : c.x - size / 2;
            Point const apex_pt{apex, c.y};
            Point const top{base, c.y - size};
            Point const bottom{base, c.y + size};
            canvas.draw_line(top, apex_pt, light);
            canvas.draw_line(apex_pt, bottom, dark);
            canvas.draw_line(top, bottom, dark);
        }
    }

    float scale_ = 1.0f;
};

std::unique_ptr<Theme> Theme::create_xen(float scale) {
    return std::make_unique<XENTheme>(scale);
}

std::unique_ptr<Theme> Theme::create_high_contrast(float scale) {
    // TODO: High contrast theme
    return create_xen(scale);
}

} // namespace aegir::trinket