/*
 * Trinket Label implementation.
 */

#include <aegir/trinket/label.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/unicode.h>
#include <algorithm>

namespace aegir::trinket {

Label::Label(std::u32string_view text) : text_(text) {}
Label::Label(std::string_view text) : text_(utf8_to_utf32(text)) {}

Label::~Label() = default;

void Label::set_text(std::u32string_view text) {
    text_ = std::u32string(text);
    text_dirty_ = true;
    damage();
}

void Label::set_text(std::string_view text) {
    text_ = utf8_to_utf32(text);
    text_dirty_ = true;
    damage();
}

void Label::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);

    /* A label with no font of its own uses the application's, the way a
     * TextBox and a Button already do; otherwise a client that never calls
     * set_font draws nothing. */
    Font* const font = font_ ? font_ : Application::instance()->default_font();
    if (text_.empty() || font == nullptr) return;

    // Elide text if needed
    if (text_dirty_ || elided_text_.empty()) {
        if (ellipsis_ && word_wrap_ == WordWrap::NONE) {
            Size text_size = font->measure(text_);
            if (text_size.width > rect_.width) {
                // Simple elision: truncate and add ...
                std::u32string ellipsis = U"...";
                Size ellipsis_size = font->measure(ellipsis);
                int available = rect_.width - ellipsis_size.width;
                if (available > 0) {
                    elided_text_.clear();
                    int width = 0;
                    for (char32_t cp : text_) {
                        const Glyph* g = font->glyph(cp);
                        int adv = g ? g->advance : 0;
                        if (width + adv > available) break;
                        elided_text_.push_back(cp);
                        width += adv;
                    }
                    elided_text_ += ellipsis;
                } else {
                    elided_text_ = ellipsis;
                }
            } else {
                elided_text_ = text_;
            }
        } else {
            elided_text_ = text_;
        }
        text_dirty_ = false;
    }

    const std::u32string& display_text = elided_text_.empty() ? text_ : elided_text_;
    if (display_text.empty()) return;

    // Calculate position based on alignment
    Size text_size = font->measure(display_text);
    int x = rect_.x;
    if (alignment_ == Alignment::CENTER) {
        x += (rect_.width - text_size.width) / 2;
    } else if (alignment_ == Alignment::RIGHT) {
        x += rect_.width - text_size.width;
    }
    int y = rect_.y + (rect_.height - font->height()) / 2;

    canvas.draw_text({x, y}, display_text, font, text_color_, BidiDirection::LTR);
}

Size Label::preferred_size() const {
    Font* const font = font_ ? font_ : Application::instance()->default_font();
    if (font == nullptr || text_.empty()) return {0, font ? font->height() : 0};
    return font->measure(text_);
}

} // namespace aegir::trinket