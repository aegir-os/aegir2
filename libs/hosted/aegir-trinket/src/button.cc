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

    Font* const font = Application::instance()->default_font();
    if (type_ == Type::CHECK || type_ == Type::RADIO) {
        /* The indicator is the theme's artwork -- MUI's checkmark button or its
         * radio ring, in the unchecked or checked frame
         * (specs/trinket/checkbox.md) -- at the left and centred vertically;
         * the label sits to its right. */
        bool const radio = type_ == Type::RADIO;
        Size const indicator{
            theme.metric(radio ? MetricRole::RADIO_INDICATOR_WIDTH
                               : MetricRole::CHECK_INDICATOR_WIDTH),
            theme.metric(radio ? MetricRole::RADIO_INDICATOR_HEIGHT
                               : MetricRole::CHECK_INDICATOR_HEIGHT)};
        Rect const box{r.x, r.y + (r.height - indicator.height) / 2,
                       indicator.width, indicator.height};
        if (radio) {
            theme.draw_radio(canvas, box, checked_, enabled_);
        } else {
            theme.draw_check(canvas, box, checked_, enabled_);
        }
        if (!text_.empty() && font != nullptr) {
            Color const ink = enabled_ ? theme.color(ColorRole::BUTTON_TEXT)
                                       : theme.color(ColorRole::DISABLED_TEXT);
            int const x = box.x + box.width + theme.metric(MetricRole::SPACING_SMALL);
            int const y = r.y + (r.height - font->height()) / 2;
            canvas.draw_text({x, y}, text_, font, ink);
        }
        return;
    }

    /* The button's frame is the theme's (specs/trinket/theme-xen.md): a raised
     * #bfbfbf face with a 1px bevel, not a rounded rect drawn here. */
    theme.draw_button(canvas, r, hovered_, pressed_, focused_, checked_, enabled_);
    if (!text_.empty() && font != nullptr) {
        Color const text_color = enabled_ ? theme.color(ColorRole::BUTTON_TEXT)
                                          : theme.color(ColorRole::DISABLED_TEXT);
        Size const text_size = font->measure(text_);
        int const x = r.x + (r.width - text_size.width) / 2;
        int const y = r.y + (r.height - font->height()) / 2;
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
        /* A toggle took its new state on the press; a push reports on the
         * release. Either way the click is reported, as the key path (on_key_up)
         * does too -- a toggle whose click was never reported is a toggle whose
         * group never heard it (specs/trinket/radio_group.md). */
        if (on_click) on_click(checked_);
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

    Theme& theme = Application::instance()->theme();
    Size const text_size = font->measure(text_);
    int const padding_h = theme.metric(MetricRole::BUTTON_PADDING_H);
    int const padding_v = theme.metric(MetricRole::BUTTON_PADDING_V);

    if (type_ == Type::CHECK || type_ == Type::RADIO) {
        /* The indicator, the gap and the label (specs/trinket/checkbox.md). A
         * toggle with no text is the indicator alone: the padding and the gap
         * frame a label, and carrying them for an empty one makes an inline
         * checkbox taller than the field it sits beside. */
        bool const radio = type_ == Type::RADIO;
        int const indicator_w =
            theme.metric(radio ? MetricRole::RADIO_INDICATOR_WIDTH
                               : MetricRole::CHECK_INDICATOR_WIDTH);
        int const indicator_h =
            theme.metric(radio ? MetricRole::RADIO_INDICATOR_HEIGHT
                               : MetricRole::CHECK_INDICATOR_HEIGHT);
        if (text_.empty()) {
            return {indicator_w, indicator_h};
        }
        int const gap = theme.metric(MetricRole::SPACING_SMALL);
        return {indicator_w + gap + text_size.width + 2 * padding_h,
                std::max(indicator_h, text_size.height) + 2 * padding_v};
    }

    int const min_w = theme.metric(MetricRole::BUTTON_MIN_WIDTH);
    int const min_h = theme.metric(MetricRole::BUTTON_MIN_HEIGHT);
    return {std::max(min_w, text_size.width + 2 * padding_h),
            std::max(min_h, text_size.height + 2 * padding_v)};
}

} // namespace aegir::trinket