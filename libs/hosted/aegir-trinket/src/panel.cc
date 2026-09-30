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

    /* A flat panel with its own background keeps it; every frame is the
     * theme's (specs/trinket/theme-xen.md). In the trees in use the flat
     * background is PANEL_BG's own value, so this is the same pixel either
     * way. */
    if (style_ == Style::FLAT && background_.a > 0) {
        canvas.fill_rect(rect_, background_);
    } else {
        theme.draw_panel(canvas, rect_, style_, title_, focused());
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