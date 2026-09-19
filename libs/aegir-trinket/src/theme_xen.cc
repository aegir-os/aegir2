/*
 * Trinket XEN/Workbench theme implementation.
 */

#include <aegir/trinket/theme.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/application.h>
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
            case CR::WINDOW_BG: return Color(0x00CCCCCC);        // Light grey
            case CR::PANEL_BG: return Color(0x00DDDDDD);         // Lighter grey

            // Text
            case CR::TEXT: return Color(0x00000000);             // Black
            case CR::TEXT_DISABLED: return Color(0x00808080);    // Grey
            case CR::TEXT_SELECTED: return Color(0x00FFFFFF);    // White
            case CR::TEXT_INVERSE: return Color(0x00FFFFFF);

            // Buttons
            case CR::BUTTON_BG: return Color(0x00E0E0E0);        // Button grey
            case CR::BUTTON_HOVER: return Color(0x00D0D0D0);
            case CR::BUTTON_PRESSED: return Color(0x00B0B0B0);
            case CR::BUTTON_FOCUS: return Color(0x000078D7);     // Blue focus
            case CR::BUTTON_TEXT: return Color(0x00000000);
            case CR::BUTTON_BORDER: return Color(0x00808080);

            // Input
            case CR::INPUT_BG: return Color(0x00FFFFFF);
            case CR::INPUT_TEXT: return Color(0x00000000);
            case CR::INPUT_PLACEHOLDER: return Color(0x00808080);
            case CR::INPUT_BORDER: return Color(0x00808080);
            case CR::INPUT_FOCUS_BORDER: return Color(0x000078D7);

            // Selection
            case CR::SELECTION_BG: return Color(0x000078D7);
            case CR::SELECTION_TEXT: return Color(0x00FFFFFF);

            // Accent
            case CR::ACCENT: return Color(0x000078D7);           // Blue
            case CR::ACCENT_HOVER: return Color(0x001084E8);
            case CR::ACCENT_PRESSED: return Color(0x00005A9E);

            // Borders
            case CR::BORDER: return Color(0x00808080);
            case CR::BORDER_LIGHT: return Color(0x00FFFFFF);
            case CR::BORDER_DARK: return Color(0x00404040);
            case CR::FOCUS_BORDER: return Color(0x000078D7);

            // Menu
            case CR::MENUBAR_BG: return Color(0x00E0E0E0);
            case CR::MENUBAR_HOVER: return Color(0x000078D7);
            case CR::MENUBAR_TEXT: return Color(0x00000000);
            case CR::MENU_BG: return Color(0x00F0F0F0);
            case CR::MENU_HOVER: return Color(0x000078D7);
            case CR::MENU_TEXT: return Color(0x00000000);
            case CR::MENU_BORDER: return Color(0x00808080);
            case CR::MENU_SEPARATOR: return Color(0x00808080);

            // Titlebar
            case CR::TITLEBAR_BG: return Color(0x000078D7);      // Active blue
            case CR::TITLEBAR_BG_INACTIVE: return Color(0x00E0E0E0);
            case CR::TITLEBAR_TEXT: return Color(0x00FFFFFF);
            case CR::TITLEBAR_TEXT_INACTIVE: return Color(0x00000000);
            case CR::TITLEBAR_BUTTON_BG: return Color::TRANSPARENT;
            case CR::TITLEBAR_BUTTON_HOVER: return Color(0x40FFFFFF);

            // Tooltip
            case CR::TOOLTIP_BG: return Color(0xE0FFFF80);       // Yellow
            case CR::TOOLTIP_TEXT: return Color(0x00000000);

            // Scrollbar
            case CR::SCROLLBAR_BG: return Color(0x00E0E0E0);
            case CR::SCROLLBAR_HANDLE: return Color(0x00808080);
            case CR::SCROLLBAR_HANDLE_HOVER: return Color(0x00606060);

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

    void draw_button(Canvas& canvas, const Rect& rect,
                     bool hovered, bool pressed, bool focused,
                     bool checked, bool enabled) override {
        Color bg = enabled ? (pressed ? color(ColorRole::BUTTON_PRESSED)
                                      : hovered ? color(ColorRole::BUTTON_HOVER)
                                                : color(ColorRole::BUTTON_BG))
                           : color(ColorRole::DISABLED_BG);

        canvas.fill_rounded_rect(rect, metric(MetricRole::BUTTON_RADIUS), bg);

        Color border = focused ? color(ColorRole::FOCUS_BORDER)
                              : color(ColorRole::BUTTON_BORDER);
        canvas.draw_rounded_rect(rect, metric(MetricRole::BUTTON_RADIUS), border,
                                 metric(MetricRole::BUTTON_BORDER_WIDTH));
    }

    void draw_panel(Canvas& canvas, const Rect& rect,
                     Panel::Style style, bool focused) override {
        Color bg = color(ColorRole::PANEL_BG);
        Color border = color(ColorRole::BORDER);
        int bw = metric(MetricRole::PANEL_BORDER_WIDTH);

        switch (style) {
            case Panel::Style::FLAT:
                canvas.fill_rect(rect, bg);
                break;
            case Panel::Style::RAISED:
                canvas.fill_rect(rect, bg);
                canvas.draw_hline(rect.x, rect.x + rect.width - 1, rect.y, Color::WHITE);
                canvas.draw_vline(rect.y, rect.y + rect.height - 1, rect.x, Color::WHITE);
                canvas.draw_hline(rect.x + 1, rect.x + rect.width - 1, rect.y + rect.height - 1, Color::DARK_GRAY);
                canvas.draw_vline(rect.y + 1, rect.y + rect.height - 1, rect.x + rect.width - 1, Color::DARK_GRAY);
                break;
            case Panel::Style::SUNKEN:
                canvas.fill_rect(rect, bg);
                canvas.draw_hline(rect.x, rect.x + rect.width - 1, rect.y, Color::DARK_GRAY);
                canvas.draw_vline(rect.y, rect.y + rect.height - 1, rect.x, Color::DARK_GRAY);
                canvas.draw_hline(rect.x + 1, rect.x + rect.width - 1, rect.y + rect.height - 1, Color::WHITE);
                canvas.draw_vline(rect.y + 1, rect.y + rect.height - 1, rect.x + rect.width - 1, Color::WHITE);
                break;
            case Panel::Style::FRAME:
                canvas.fill_rect(rect, bg);
                canvas.draw_rect(rect, border, bw);
                break;
            case Panel::Style::GROUP_BOX:
                // Handled in Panel::on_paint
                break;
        }
    }

    void draw_textbox(Canvas& canvas, const Rect& rect,
                       bool focused, bool read_only, bool password) override {
        Color bg = read_only ? color(ColorRole::DISABLED_BG) : color(ColorRole::INPUT_BG);
        Color border = focused ? color(ColorRole::INPUT_FOCUS_BORDER)
                              : color(ColorRole::INPUT_BORDER);
        int bw = metric(MetricRole::INPUT_BORDER_WIDTH);
        int radius = 2;

        canvas.fill_rounded_rect(rect, radius, bg);
        canvas.draw_rounded_rect(rect, radius, border, bw);
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
            int y = rect.y + (rect.height + font->ascent() - font->descent()) / 2;
            canvas.draw_text({x, y}, std::u32string(reinterpret_cast<const char32_t*>(label)),
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
            int y = rect.y + (rect.height + font->ascent() - font->descent()) / 2;
            canvas.draw_text({x, y}, std::u32string(reinterpret_cast<const char32_t*>(title)),
                             font, text);
        }
    }

    void draw_window_frame(Canvas& canvas, const Rect& rect, bool active) override {
        Color border = active ? color(ColorRole::FOCUS_BORDER) : color(ColorRole::BORDER);
        int bw = metric(MetricRole::WINDOW_BORDER_WIDTH);
        canvas.draw_rect(rect, border, bw);
    }

    void draw_scrollbar(Canvas& canvas, const Rect& rect,
                         bool vertical, int handle_pos, int handle_size,
                         bool hovered) override {
        Color bg = color(ColorRole::SCROLLBAR_BG);
        Color handle = hovered ? color(ColorRole::SCROLLBAR_HANDLE_HOVER)
                              : color(ColorRole::SCROLLBAR_HANDLE);
        canvas.fill_rect(rect, bg);

        Rect handle_rect = vertical
            ? Rect{rect.x, rect.y + handle_pos, rect.width, handle_size}
            : Rect{rect.x + handle_pos, rect.y, handle_size, rect.height};
        canvas.fill_rounded_rect(handle_rect, rect.width / 2, handle);
    }

    void draw_focus_ring(Canvas& canvas, const Rect& rect) override {
        int w = metric(MetricRole::FOCUS_RING_WIDTH);
        int offset = metric(MetricRole::FOCUS_RING_OFFSET);
        Rect r = rect.inflated(offset);
        canvas.draw_rounded_rect(r, 4, color(ColorRole::FOCUS_BORDER), w);
    }

    void draw_tooltip(Canvas& canvas, const Rect& rect,
                       const char* text) override {
        canvas.fill_rounded_rect(rect, 4, color(ColorRole::TOOLTIP_BG));
        Font* font = Application::instance()->default_font();
        if (font && text) {
            int x = rect.x + 6;
            int y = rect.y + (rect.height + font->ascent() - font->descent()) / 2;
            canvas.draw_text({x, y}, std::u32string(reinterpret_cast<const char32_t*>(text)),
                             font, color(ColorRole::TOOLTIP_TEXT));
        }
    }

private:
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