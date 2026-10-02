/*
 * Trinket TabGroup implementation (see the header).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/tab_group.h>

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/theme.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace aegir::trinket {

TabGroup::TabGroup() = default;

TabGroup::~TabGroup() = default;

void TabGroup::add_page(std::u32string_view title, std::unique_ptr<Widget> page)
{
    if (page == nullptr) {
        return;
    }
    Widget* const raw = page.get();
    /* The page is a child, and the group's own layout is what arranges it. */
    Container::add_child(std::move(page));
    /* The first page is the one that shows; the rest wait hidden, so the
     * layout, the focus walk and the paint skip them. */
    bool const first = active_ < 0;
    raw->set_visible(first);
    pages_.push_back({std::u32string(title), raw});
    if (first) {
        active_ = 0;
    }
    damage();
}

Widget* TabGroup::page(int index) const
{
    if (index < 0 || index >= page_count()) {
        return nullptr;
    }
    return pages_[static_cast<size_t>(index)].widget;
}

std::u32string const& TabGroup::title(int index) const
{
    static std::u32string const kNone;
    if (index < 0 || index >= page_count()) {
        return kNone;
    }
    return pages_[static_cast<size_t>(index)].title;
}

void TabGroup::set_title(int index, std::u32string_view title)
{
    if (index < 0 || index >= page_count()) {
        return;
    }
    /* A different width is a different strip, so the rename damages the whole
     * group rather than one tab's rectangle. */
    pages_[static_cast<size_t>(index)].title = std::u32string(title);
    damage();
}

void TabGroup::set_active(int index)
{
    if (index < 0 || index >= page_count() || index == active_) {
        return;
    }
    if (Widget* const was = page(active_)) {
        was->set_visible(false);
    }
    active_ = index;
    if (Widget* const now = page(active_)) {
        now->set_visible(true);
    }
    damage();
}

void TabGroup::choose(int index)
{
    if (index < 0 || index >= page_count() || index == active_) {
        return;
    }
    set_active(index);
    if (on_change) {
        on_change(active_);
    }
}

TabGroup::TabLayout TabGroup::tab_layout(std::vector<int> const& title_widths,
                                         int padding, int gap)
{
    TabLayout layout;
    int x = 0;
    for (int const title : title_widths) {
        int const width = title + 2 * padding;
        layout.x.push_back(x);
        layout.width.push_back(width > 0 ? width : 0);
        x += width + gap;
    }
    return layout;
}

int TabGroup::tab_at(TabLayout const& layout, int x)
{
    for (size_t i = 0; i < layout.x.size(); ++i) {
        if (x >= layout.x[i] && x < layout.x[i] + layout.width[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<int> TabGroup::measured_title_widths() const
{
    std::vector<int> widths;
    widths.reserve(pages_.size());
    Font* const font = Application::instance() != nullptr
                           ? Application::instance()->default_font()
                           : nullptr;
    for (Page const& page : pages_) {
        widths.push_back(font != nullptr ? font->measure(page.title).width : 0);
    }
    return widths;
}

Rect TabGroup::body_rect() const
{
    int const strip = Application::instance()->theme().metric(MetricRole::TAB_HEIGHT);
    /* The strip's last row is the frame's top border, so the active tab (whose
     * bottom edge is left open) meets the body with no line between them. */
    int const height = rect_.height - strip + 1;
    return {rect_.x, rect_.y + strip - 1, rect_.width, height > 0 ? height : 0};
}

void TabGroup::on_layout()
{
    Theme& theme = Application::instance()->theme();
    int const border = theme.metric(MetricRole::PANEL_BORDER_WIDTH);
    if (Widget* const active = page(active_)) {
        active->set_rect(body_rect().inflated(-border));
    }
    Container::on_layout();
}

void TabGroup::on_paint(Canvas& canvas, const PaintEvent& event)
{
    Theme& theme = Application::instance()->theme();
    Rect const body = body_rect();
    /* The framed body, then the active page (the child), then the tabs over the
     * frame's top border -- the active one's open edge there. */
    theme.draw_panel(canvas, body, Panel::Style::FRAME, {}, false);
    Container::on_paint(canvas, event);
    int const padding = theme.metric(MetricRole::TAB_PADDING_H);
    int const gap = theme.metric(MetricRole::TAB_GAP);
    int const strip = theme.metric(MetricRole::TAB_HEIGHT);
    TabLayout const layout = tab_layout(measured_title_widths(), padding, gap);
    for (int i = 0; i < page_count(); ++i) {
        Rect const tab{rect_.x + layout.x[static_cast<size_t>(i)], rect_.y,
                       layout.width[static_cast<size_t>(i)], strip};
        theme.draw_tab(canvas, tab, pages_[static_cast<size_t>(i)].title, i == active_,
                       i == hovered_);
    }
}

void TabGroup::on_mouse_down(const MouseEvent& event)
{
    if (event.button != MouseButton::LEFT) {
        return;
    }
    Theme& theme = Application::instance()->theme();
    int const strip = theme.metric(MetricRole::TAB_HEIGHT);
    if (event.pos.y < rect_.y || event.pos.y >= rect_.y + strip) {
        return;
    }
    TabLayout const layout = tab_layout(
        measured_title_widths(), theme.metric(MetricRole::TAB_PADDING_H),
        theme.metric(MetricRole::TAB_GAP));
    int const index = tab_at(layout, event.pos.x - rect_.x);
    if (index >= 0) {
        choose(index);
    }
}

void TabGroup::on_mouse_move(const MouseEvent& event)
{
    Theme& theme = Application::instance()->theme();
    int const strip = theme.metric(MetricRole::TAB_HEIGHT);
    int index = -1;
    if (event.pos.y >= rect_.y && event.pos.y < rect_.y + strip) {
        TabLayout const layout = tab_layout(
            measured_title_widths(), theme.metric(MetricRole::TAB_PADDING_H),
            theme.metric(MetricRole::TAB_GAP));
        index = tab_at(layout, event.pos.x - rect_.x);
    }
    if (index != hovered_) {
        hovered_ = index;
        damage();
    }
}

void TabGroup::on_mouse_leave(const MouseEvent&)
{
    if (hovered_ != -1) {
        hovered_ = -1;
        damage();
    }
}

void TabGroup::on_key_down(const KeyEvent& event)
{
    if (!event.pressed) {
        return;
    }
    int next = active_;
    switch (event.code) {
    case KeyCode::LEFT:
        next = active_ - 1;
        break;
    case KeyCode::RIGHT:
        next = active_ + 1;
        break;
    case KeyCode::HOME:
        next = 0;
        break;
    case KeyCode::END:
        next = page_count() - 1;
        break;
    default:
        return;
    }
    if (next >= 0 && next < page_count()) {
        choose(next);
    }
}

Size TabGroup::preferred_size() const
{
    Theme& theme = Application::instance()->theme();
    int const padding = theme.metric(MetricRole::TAB_PADDING_H);
    int const gap = theme.metric(MetricRole::TAB_GAP);
    int const strip = theme.metric(MetricRole::TAB_HEIGHT);
    int const border = theme.metric(MetricRole::PANEL_BORDER_WIDTH);
    std::vector<int> const widths = measured_title_widths();
    int tabs = 0;
    for (size_t i = 0; i < widths.size(); ++i) {
        tabs += widths[i] + 2 * padding + (i == 0 ? 0 : gap);
    }
    Size content{0, 0};
    if (Widget* const active = page(active_)) {
        content = active->preferred_size();
    }
    return {std::max(tabs, content.width + 2 * border), strip + content.height + 2 * border};
}

Size TabGroup::maximum_size() const
{
    /* The group fills the room it is given. It has no `Layout` to answer a
     * maximum, so without this `Container::maximum_size` would fall back to the
     * minimum and a host could not stretch it -- the page would sit at its
     * least and leave the body empty (specs/trinket/tabs.md). */
    int const big = std::numeric_limits<int>::max();
    return {big, big};
}

Size TabGroup::minimum_size() const
{
    Theme& theme = Application::instance()->theme();
    int const strip = theme.metric(MetricRole::TAB_HEIGHT);
    int const border = theme.metric(MetricRole::PANEL_BORDER_WIDTH);
    Size content{0, 0};
    if (Widget* const active = page(active_)) {
        content = active->minimum_size();
    }
    return {content.width + 2 * border, strip + content.height + 2 * border};
}

}  // namespace aegir::trinket
