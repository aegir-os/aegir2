/*
 * Trinket Panel implementation.
 */

#include <aegir/trinket/panel.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>

namespace aegir::trinket {

Panel::Panel(Style style) : style_(style) {
    set_layout(std::make_unique<FlowLayout>(FlowLayout::Direction::VERTICAL, 4));
}

Panel::~Panel() = default;

void Panel::set_title(std::u32string_view title) {
    title_ = std::u32string(title);
    damage();
}

void Panel::set_title(std::string_view title) {
    title_ = utf8_to_utf32(title);
    damage();
}

void Panel::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);

    Theme& theme = Application::instance()->theme();
    Rect r = rect_;

    Color bg = background_.a > 0 ? background_ : theme.color(ColorRole::PANEL_BG);
    int bw = border_width_;

    switch (style_) {
        case Style::FLAT:
            canvas.fill_rect(r, bg);
            break;
        case Style::RAISED:
            canvas.fill_rect(r, bg);
            // Top/left highlight
            canvas.draw_hline(r.x, r.x + r.width - 1, r.y, Color::WHITE);
            canvas.draw_vline(r.y, r.y + r.height - 1, r.x, Color::WHITE);
            // Bottom/right shadow
            canvas.draw_hline(r.x + 1, r.x + r.width - 1, r.y + r.height - 1, Color::DARK_GRAY);
            canvas.draw_vline(r.y + 1, r.y + r.height - 1, r.x + r.width - 1, Color::DARK_GRAY);
            break;
        case Style::SUNKEN:
            canvas.fill_rect(r, bg);
            // Top/left shadow
            canvas.draw_hline(r.x, r.x + r.width - 1, r.y, Color::DARK_GRAY);
            canvas.draw_vline(r.y, r.y + r.height - 1, r.x, Color::DARK_GRAY);
            // Bottom/right highlight
            canvas.draw_hline(r.x + 1, r.x + r.width - 1, r.y + r.height - 1, Color::WHITE);
            canvas.draw_vline(r.y + 1, r.y + r.height - 1, r.x + r.width - 1, Color::WHITE);
            break;
        case Style::FRAME:
            canvas.fill_rect(r, bg);
            canvas.draw_rect(r, Application::instance()->theme().color(ColorRole::BORDER), bw);
            break;
        case Style::GROUP_BOX:
            if (!title_.empty()) {
                // Draw frame with title gap
                Font* font = Application::instance()->default_font();
                Size title_size = font ? font->measure(title_) : Size{0, 0};
                int gap_x = 8;
                int title_w = title_size.width;

                // Top line with title gap
                canvas.draw_hline(r.x, r.x + gap_x - 1, r.y, Application::instance()->theme().color(ColorRole::BORDER));
                canvas.draw_hline(r.x + gap_x + title_w + 4, r.x + r.width - 1, r.y,
                                  Application::instance()->theme().color(ColorRole::BORDER));
                // Title text
                if (font) {
                    canvas.draw_text({r.x + gap_x + 2, r.y - font->ascent() / 2},
                                     title_, font, Application::instance()->theme().color(ColorRole::TEXT));
                }
                // Other sides
                canvas.draw_vline(r.y, r.y + r.height - 1, r.x, Application::instance()->theme().color(ColorRole::BORDER));
                canvas.draw_vline(r.y, r.y + r.height - 1, r.x + r.width - 1, Application::instance()->theme().color(ColorRole::BORDER));
                canvas.draw_hline(r.x, r.x + r.width - 1, r.y + r.height - 1, Application::instance()->theme().color(ColorRole::BORDER));
            } else {
                canvas.draw_rect(r, Application::instance()->theme().color(ColorRole::BORDER));
            }
            break;
    }

    // Paint children
    Container::on_paint(canvas, event);
}

Size Panel::preferred_size() const {
    Size child_size = Container::preferred_size();
    int bw = border_width_;
    return {child_size.width + 2 * bw, child_size.height + 2 * bw};
}

} // namespace aegir::trinket