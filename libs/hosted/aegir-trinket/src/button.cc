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

Button::Button(std::string_view text, Type type) : text_(utf8_to_utf32(text)), type_(type) {}

Button::~Button() = default;

void Button::set_text(std::u32string_view text) {
    text_ = std::u32string(text);
    damage();
}

void Button::set_text(std::string_view text) {
    text_ = utf8_to_utf32(text);
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

    if (type_ == Type::CHECK || type_ == Type::RADIO) {
        /* The indicator only -- a checkmark's square well or a radio's round
         * one, with its mark when checked (specs/trinket/theme-xen.md). */
        int side = std::min(r.width, r.height) - 4;
        if (side < 1) side = 1;
        Rect const box{r.x + 2, r.y + (r.height - side) / 2, side, side};
        Color const ink = enabled_ ? theme.color(ColorRole::TEXT)
                                   : theme.color(ColorRole::DISABLED_TEXT);
        if (type_ == Type::RADIO) {
            canvas.fill_circle(box.center(), side / 2,
                               theme.color(ColorRole::INPUT_BG));
            canvas.draw_circle(box.center(), side / 2, ink);
            if (checked_) {
                canvas.fill_circle(box.center(), std::max(1, side / 4),
                                   theme.color(ColorRole::ACCENT));
            }
        } else {
            canvas.fill_rect(box, theme.color(ColorRole::INPUT_BG));
            theme.draw_bevel(canvas, box, Theme::Bevel::SUNKEN);
            if (checked_) {
                canvas.draw_line({box.x + 3, box.y + side / 2},
                                 {box.x + side / 2, box.y + side - 4}, ink);
                canvas.draw_line({box.x + side / 2, box.y + side - 4},
                                 {box.x + side - 3, box.y + 3}, ink);
            }
        }
    } else {
        /* The button's frame is the theme's (specs/trinket/theme-xen.md): a
         * raised #bfbfbf face with a 1px bevel, not a rounded rect drawn
         * here. */
        theme.draw_button(canvas, r, hovered_, pressed_, focused_, checked_, enabled_);
    }

    // Text
    Font* font = Application::instance()->default_font();
    if (!text_.empty() && font) {
        Color text_color = enabled_ ? theme.color(ColorRole::BUTTON_TEXT)
                                    : theme.color(ColorRole::DISABLED_TEXT);
        Size text_size = font->measure(text_);
        int x = r.x + (r.width - text_size.width) / 2;
        int y = r.y + (r.height - font->height()) / 2;
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