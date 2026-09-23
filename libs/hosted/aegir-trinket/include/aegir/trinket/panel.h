/*
 * Trinket Panel widget.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_PANEL_H
#define AEGIR_TRINKET_PANEL_H

#include <aegir/trinket/widget.h>
#include <aegir/trinket/layout.h>
#include <aegir/trinket/unicode.h>

namespace aegir::trinket {

class Panel : public Container {
public:
    enum class Style { FLAT, RAISED, SUNKEN, FRAME, GROUP_BOX };

    Panel(Style style = Style::FLAT);
    ~Panel() override;

    void set_style(Style s) { style_ = s; damage(); }
    Style style() const { return style_; }

    void set_border_width(int w) { border_width_ = w; damage(); }
    int border_width() const { return border_width_; }

    void set_background(Color c) { background_ = c; damage(); }
    Color background() const { return background_; }

    // For GROUP_BOX style
    void set_title(std::u32string_view title);
    void set_title(std::string_view title);
    const std::u32string& title() const { return title_; }

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    Size preferred_size() const override;

private:
    Style style_ = Style::FLAT;
    int border_width_ = 1;
    Color background_ = Color::TRANSPARENT;
    std::u32string title_;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_PANEL_H