/*
 * Trinket Button implementation.
 */

#include <aegir/trinket/button.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>

namespace aegir::trinket {

Button::Button(std::u32string_view text, Type type)
    : text_(text), type_(type) {}

Button::Button(std::string_view text, Type type) : type_(type), text_(utf8_to_utf32(text)) {}

Button::~Button() = default;

void Button::set_text(std::u32string_view text) {
    text_ = text;
    damage();
}

void Button::set_text(std::string_view text) {
    text_ = utf8_to_utf32(text);
    damage();
}

void Button::set_icon(std::u32string_view icon) {
    icon_ = icon;
    damage();
}

void Button::set_icon(std::string_view icon) {
    icon_ = utf8_to_utf32(icon);
    damage();
}

void Button::set_auto_repeat(bool enable, int interval_ms) {
    auto_repeat_ = enable;
    repeat_interval_ = interval_ms;
}

void Button::on_paint(Canvas& canvas, const PaintEvent&) {
    Widget::on_paint(canvas, PaintEvent{rect_});

    Theme& theme = Application::instance()->theme();
    Rect r = rect_;

    // Button background
    Color bg = theme.color(ColorRole::BUTTON_BG);
    if (pressed_) bg = theme.color(ColorRole::BUTTON_PRESSED);
    else if (hovered_) bg = theme.color(ColorRole::BUTTON_HOVER);
    if (!enabled_) bg = theme.color(ColorRole::DISABLED_BG);

    canvas.fill_rounded_rect(r, theme.metric(MetricRole::BUTTON_RADIUS), bg);

    // Border
    Color border = focused_ ? theme.color(ColorRole::FOCUS_BORDER)
                            : theme.color(ColorRole::BORDER);
    if (type_ == Type::CHECK || type_ == Type::RADIO) {
        // Draw checkbox/radio indicator
        int size = std::min(r.width, r.height) - 4;
        int ix = r.x + 4;
        int iy = r.y + (r.height - size) / 2;
        Rect ir = {ix, iy, size, size};
        canvas.draw_rounded_rect(ir, 2, border);
        if (checked_) {
            canvas.fill_rounded_rect(ir.inflated(-2), 1, theme.color(ColorRole::ACCENT));
        }
    } else {
        canvas.draw_rounded_rect(r, theme.metric(MetricRole::BUTTON_RADIUS), border);
        if (pressed_) {
            canvas.draw_rounded_rect(r.inflated(-1), theme.metric(MetricRole::BUTTON_RADIUS) - 1,
                                     theme.color(ColorRole::BUTTON_PRESSED));
        }
    }

    // Focus ring
    if (focused_) {
        theme.draw_focus_ring(canvas, r);
    }

    // Text
    Font* font = Application::instance()->default_font();
    if (!text_.empty() && font) {
        Color text_color = enabled_ ? theme.color(ColorRole::BUTTON_TEXT)
                                    : theme.color(ColorRole::DISABLED_TEXT);
        Size text_size = font->measure(text_);
        int x = r.x + (r.width - text_size.width) / 2;
        int y = r.y + (r.height + font->ascent() - font->descent()) / 2;
        canvas.draw_text({x, y}, text_, font, text_color);
    }
}

void Button::on_mouse_down(const MouseEvent& event) {
    if (!enabled_) return;
    if (event.button == MouseButton::LEFT) {
        pressed_ = true;
        damage();
        if (on_pressed) on_pressed();
        if (type_ == Type::TOGGLE || type_ == Type::CHECK || type_ == Type::RADIO) {
            set_checked(!checked_);
        }
    }
}

void Button::on_mouse_up(const MouseEvent& event) {
    if (!enabled_) return;
    if (pressed_ && event.button == MouseButton::LEFT) {
        pressed_ = false;
        damage();
        if (on_released) on_released();
        if (type_ == Type::PUSH && on_click) {
            on_click(checked_);
        }
    }
}

void Button::on_mouse_enter(const MouseEvent&) {
    if (enabled_) { hovered_ = true; damage(); }
}

void Button::on_mouse_leave(const MouseEvent&) {
    if (hovered_) { hovered_ = false; damage(); }
}

void Button::on_key_down(const KeyEvent& event) {
    if (!enabled_) return;
    if (event.code == KeyCode::SPACE || event.code == KeyCode::ENTER) {
        pressed_ = true;
        damage();
    }
}

void Button::on_key_up(const KeyEvent& event) {
    if (!enabled_) return;
    if (pressed_ && (event.code == KeyCode::SPACE || event.code == KeyCode::ENTER)) {
        pressed_ = false;
        damage();
        if (type_ == Type::PUSH && on_click) {
            on_click(checked_);
        } else if ((type_ == Type::TOGGLE || type_ == Type::CHECK || type_ == Type::RADIO) && on_click) {
            on_click(checked_);
        }
    }
}

void Button::on_focus_gained() { damage(); }
void Button::on_focus_lost() { pressed_ = false; damage(); }

Size Button::preferred_size() const {
    Font* font = Application::instance()->default_font();
    if (!font) return {80, 28};

    Size text_size = font->measure(text_);
    Theme& theme = Application::instance()->theme();
    int padding_h = theme.metric(MetricRole::BUTTON_PADDING_H);
    int padding_v = theme.metric(MetricRole::BUTTON_PADDING_V);
    int min_w = theme.metric(MetricRole::BUTTON_MIN_WIDTH);
    int min_h = theme.metric(MetricRole::BUTTON_MIN_HEIGHT);

    return {std::max(min_w, text_size.width + 2 * padding_h),
            std::max(min_h, text_size.height + 2 * padding_v)};
}

} // namespace aegir::trinket