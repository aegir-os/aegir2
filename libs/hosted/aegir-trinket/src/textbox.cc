/*
 * Trinket TextBox implementation.
 */

#include <aegir/trinket/textbox.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/unicode.h>
#include <algorithm>

namespace aegir::trinket {

TextBox::TextBox() = default;
TextBox::~TextBox() = default;

void TextBox::set_text(std::u32string_view text) {
    text_ = std::u32string(text);
    cursor_ = std::min(cursor_, text_.size());
    damage();
}

void TextBox::set_text(std::string_view text) {
    text_ = utf8_to_utf32(text);
    cursor_ = std::min(cursor_, text_.size());
    damage();
}

std::string TextBox::text_utf8() const {
    return utf32_to_utf8(text_);
}

void TextBox::set_placeholder(std::u32string_view text) {
    placeholder_ = std::u32string(text);
    damage();
}

void TextBox::set_placeholder(std::string_view text) {
    placeholder_ = utf8_to_utf32(text);
    damage();
}

void TextBox::set_cursor_pos(size_t pos) {
    cursor_ = std::min(pos, text_.size());
    damage();
}

void TextBox::set_selection(size_t start, size_t end) {
    selection_start_ = std::min(start, text_.size());
    selection_end_ = std::min(end, text_.size());
    cursor_ = selection_end_;
    damage();
}

std::u32string TextBox::selected_text() const {
    if (!has_selection()) return U"";
    size_t start = selection_start();
    size_t end = selection_end();
    return text_.substr(start, end - start);
}

void TextBox::ensure_cursor_visible() {
    if (!font_) return;
    // TODO: Implement scrolling to keep cursor visible
}

void TextBox::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);

    Theme& theme = Application::instance()->theme();
    Font* font = font_ ? font_ : Application::instance()->default_font();
    if (!font) return;

    Rect r = rect_;
    Color bg = read_only_ ? theme.color(ColorRole::DISABLED_BG) : theme.color(ColorRole::INPUT_BG);
    Color border = focused_ ? theme.color(ColorRole::INPUT_FOCUS_BORDER) : theme.color(ColorRole::INPUT_BORDER);
    int bw = theme.metric(MetricRole::INPUT_BORDER_WIDTH);
    int radius = 2;

    canvas.fill_rounded_rect(r, radius, bg);
    canvas.draw_rounded_rect(r, radius, border, bw);

    // Text rendering
    int padding_h = theme.metric(MetricRole::INPUT_PADDING_H);
    int padding_v = theme.metric(MetricRole::INPUT_PADDING_V);
    int text_x = r.x + padding_h;
    int text_y = r.y + padding_v;

    // Selection
    if (has_selection()) {
        size_t sel_start = selection_start();
        size_t sel_end = selection_end();
        if (sel_start < sel_end) {
            std::u32string before = text_.substr(0, sel_start);
            std::u32string selected = text_.substr(sel_start, sel_end - sel_start);
            Size before_size = font->measure(before);
            Size sel_size = font->measure(selected);
            Rect sel_rect = {text_x + before_size.width, r.y + padding_v, sel_size.width, font->height()};
            canvas.fill_rect(sel_rect, theme.color(ColorRole::SELECTION_BG));
        }
    }

    // Text color
    Color text_color = read_only_ ? theme.color(ColorRole::DISABLED_TEXT) : theme.color(ColorRole::INPUT_TEXT);

    /* The secret echoes as bullets: what is typed is the credential's, not
     * the screen's (specs/trinket.md). */
    std::u32string const display =
        password_mode_ ? std::u32string(text_.size(), U'*') : text_;

    // Draw text (or placeholder)
    if (!display.empty()) {
        canvas.draw_text({text_x, text_y}, display, font, text_color);
    } else if (!placeholder_.empty()) {
        canvas.draw_text({text_x, text_y}, placeholder_, font, theme.color(ColorRole::INPUT_PLACEHOLDER));
    }

    // Cursor
    if (focused_ && !read_only_) {
        std::u32string before = display.substr(0, cursor_);
        Size before_size = font->measure(before);
        int cx = text_x + before_size.width;
        int cy = text_y;
        int ch = font->height();
        canvas.draw_vline(cy, cy + ch, cx, theme.color(ColorRole::ACCENT));
    }
}

void TextBox::on_mouse_down(const MouseEvent& event) {
    if (!enabled_ || read_only_) return;
    if (event.button == MouseButton::LEFT) {
        set_focused(true);
        // TODO: Calculate cursor position from click
    }
}

void TextBox::on_mouse_up(const MouseEvent&) {}
void TextBox::on_mouse_move(const MouseEvent&) {}

void TextBox::on_key_down(const KeyEvent& event) {
    if (!enabled_ || read_only_) return;

    bool changed = false;

    switch (event.code) {
        case KeyCode::LEFT:
            if (event.modifiers & 2) { // CTRL
                // Word left
            } else if (cursor_ > 0) {
                --cursor_;
            }
            changed = true;
            break;
        case KeyCode::RIGHT:
            if (event.modifiers & 2) { // CTRL
                // Word right
            } else if (cursor_ < text_.size()) {
                ++cursor_;
            }
            changed = true;
            break;
        case KeyCode::HOME:
            cursor_ = 0;
            changed = true;
            break;
        case KeyCode::END:
            cursor_ = text_.size();
            changed = true;
            break;
        case KeyCode::BACKSPACE:
            if (has_selection()) {
                size_t start = selection_start();
                size_t end = selection_end();
                text_.erase(start, end - start);
                cursor_ = start;
                changed = true;
            } else if (cursor_ > 0) {
                text_.erase(cursor_ - 1, 1);
                --cursor_;
                changed = true;
            }
            break;
        case KeyCode::DELETE_KEY:
            if (has_selection()) {
                size_t start = selection_start();
                size_t end = selection_end();
                text_.erase(start, end - start);
                cursor_ = start;
                changed = true;
            } else if (cursor_ < text_.size()) {
                text_.erase(cursor_, 1);
                changed = true;
            }
            break;
        case KeyCode::ENTER:
            if (multiline_) {
                text_.insert(cursor_, 1, U'\n');
                ++cursor_;
                changed = true;
            } else if (on_submit) {
                on_submit();
            }
            break;
        case KeyCode::ESCAPE:
            if (on_cancel) on_cancel();
            break;
        default:
            if (event.text != 0 && event.text >= 32 && event.text < 127) {
                if (max_length_ == 0 || text_.size() < max_length_) {
                    if (password_mode_) {
                        text_.insert(cursor_, 1, event.text);
                    } else {
                        text_.insert(cursor_, 1, event.text);
                    }
                    ++cursor_;
                    changed = true;
                }
            }
            break;
    }

    if (changed) {
        clear_selection();
        damage();
        if (on_text_changed) on_text_changed(text_);
    }
}

void TextBox::on_key_up(const KeyEvent&) {}

void TextBox::on_focus_gained() {
    damage();
}

void TextBox::on_focus_lost() {
    clear_selection();
    damage();
}

Size TextBox::preferred_size() const {
    Font* font = font_ ? font_ : Application::instance()->default_font();
    if (!font) return {100, 24};
    int padding_h = Application::instance()->theme().metric(MetricRole::INPUT_PADDING_H);
    int padding_v = Application::instance()->theme().metric(MetricRole::INPUT_PADDING_V);
    Size text_size = font->measure(text_.empty() ? placeholder_ : text_);
    return {std::max(100, text_size.width + 2 * padding_h),
            text_size.height + 2 * padding_v};
}

} // namespace aegir::trinket