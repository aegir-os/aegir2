/*
 * Trinket TabGroup widget -- a group of titled pages, one showing
 * (specs/trinket/tabs.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A `Container` whose children are its pages: exactly one is visible -- the
 * active one -- and the rest are hidden, so the group layout, the focus walk
 * and the paint all skip them already. The strip is along the top, its tabs the
 * theme's to draw; the body below it is a framed panel, and what a page paints
 * inside is the app's.
 */

#ifndef AEGIR_TRINKET_TAB_GROUP_H
#define AEGIR_TRINKET_TAB_GROUP_H

#include <aegir/trinket/widget.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aegir::trinket {

class TabGroup : public Container {
public:
    TabGroup();
    ~TabGroup() override;

    /* Append a page, titled. The first page added becomes the active one; the
     * rest wait hidden until chosen. */
    void add_page(std::u32string_view title, std::unique_ptr<Widget> page);

    int page_count() const { return static_cast<int>(pages_.size()); }
    Widget* page(int index) const;
    std::u32string const& title(int index) const;

    /* Choose the page that shows. Programmatic: it does not run `on_change`,
     * which is the user's choice (a click or the keys), as `ListView`'s
     * `set_active` and `on_select` split. */
    void set_active(int index);
    int active() const { return active_; }

    /* The user chose a tab. Not run by `set_active`. */
    std::function<void(int)> on_change;

    /* The strip's pure layout: each tab's x and width, in order, from the
     * titles' measured widths, the horizontal padding and the gap. No font, no
     * theme, so the host check pins it (specs/trinket/tabs.md). */
    struct TabLayout {
        std::vector<int> x;
        std::vector<int> width;
    };
    static TabLayout tab_layout(std::vector<int> const& title_widths, int padding,
                                int gap);
    /* The tab a point's x is over, or -1. */
    static int tab_at(TabLayout const& layout, int x);

    bool focusable() const override { return true; }

    Size preferred_size() const override;
    Size minimum_size() const override;

protected:
    void on_layout() override;
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_mouse_down(const MouseEvent& event) override;
    void on_mouse_move(const MouseEvent& event) override;
    void on_mouse_leave(const MouseEvent& event) override;
    void on_key_down(const KeyEvent& event) override;

private:
    struct Page {
        std::u32string title;
        Widget* widget = nullptr;
    };

    std::vector<int> measured_title_widths() const;
    /* The framed page area: the widget's rectangle less the strip, with the
     * strip's last row as the frame's top border. */
    Rect body_rect() const;
    void choose(int index); /* the user's choice */

    std::vector<Page> pages_;
    int active_ = -1;
    /* The tab the pointer is over, or -1. A theme may shade it; XEN does not. */
    int hovered_ = -1;
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_TAB_GROUP_H
