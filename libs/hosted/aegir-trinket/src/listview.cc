/*
 * Trinket ListView implementation (see the header for the shape).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/listview.h>

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/theme.h>

#include <algorithm>
#include <limits>

namespace aegir::trinket {

namespace {
/* The well's one-pixel outline, which the rows are set in from
 * (specs/trinket/listview.md). */
constexpr int kWell = 1;
}  // namespace

ListView::ListView() = default;

ListView::~ListView() = default;

void ListView::add(std::u32string_view text) {
    rows_.emplace_back(text);
    damage();
}

void ListView::clear() {
    rows_.clear();
    active_ = -1;
    cursor_ = -1;
    first_ = 0;
    damage();
}

const std::u32string& ListView::row(int index) const {
    static const std::u32string none;
    if (index < 0 || index >= count()) return none;
    return rows_[static_cast<size_t>(index)];
}

void ListView::set_active(int index) {
    if (index < -1) index = -1;
    if (index >= count()) index = count() - 1;
    if (index == active_) return;
    active_ = index;
    damage();
}

void ListView::set_cursor(int index) {
    if (index < -1) index = -1;
    if (index >= count()) index = count() - 1;
    if (index == cursor_) return;
    cursor_ = index;
    damage();
}

int ListView::clamp_first(int value, int count, int visible) {
    int const most = count > visible ? count - visible : 0;
    if (value < 0) return 0;
    if (value > most) return most;
    return value;
}

void ListView::set_first(int index) {
    int const clamped = clamp_first(index, count(), visible_rows());
    if (clamped == first_) return;
    first_ = clamped;
    damage();
}

int ListView::row_height() const {
    Theme& theme = Application::instance()->theme();
    Font* const font = Application::instance()->default_font();
    int const line = font != nullptr ? font->height() : 0;
    return line + 2 * theme.metric(MetricRole::LIST_ROW_PADDING_V);
}

Rect ListView::rows_rect() const {
    return rect_.inflated(-kWell);
}

int ListView::visible_rows() const {
    int const h = row_height();
    int const height = rows_rect().height;
    if (h <= 0 || height <= 0) return 0;
    return height / h;
}

int ListView::row_at(Point p) const {
    Rect const area = rows_rect();
    int const h = row_height();
    if (h <= 0 || !area.contains(p)) return -1;
    int const index = first_ + (p.y - area.y) / h;
    return index < count() ? index : -1;
}

Size ListView::preferred_size() const {
    Theme& theme = Application::instance()->theme();
    int const rows = std::max(1, theme.metric(MetricRole::LIST_PREFERRED_ROWS));
    return {theme.metric(MetricRole::LIST_MIN_WIDTH), 2 * kWell + rows * row_height()};
}

Size ListView::minimum_size() const { return preferred_size(); }

Size ListView::maximum_size() const {
    return {std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
}

void ListView::select(int index) {
    if (index < 0 || index >= count()) return;
    if (index == active_) return;
    active_ = index;
    damage();
    if (on_select) on_select(active_);
}

void ListView::ensure_visible(int index) {
    int const visible = visible_rows();
    if (visible <= 0) return;
    if (index < first_) {
        set_first(index);
    } else if (index >= first_ + visible) {
        set_first(index - visible + 1);
    }
}

void ListView::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);
    Theme& theme = Application::instance()->theme();
    theme.draw_list(canvas, rect_);
    Font* const font = Application::instance()->default_font();
    int const h = row_height();
    if (h <= 0) return;
    Rect const area = rows_rect();
    int const visible = visible_rows();
    int const pad = theme.metric(MetricRole::LIST_PADDING_H);
    for (int i = 0; i < visible; ++i) {
        int const index = first_ + i;
        if (index >= count()) break;
        Rect const row_rect{area.x, area.y + i * h, area.width, h};
        Theme::ListRow state = Theme::ListRow::NORMAL;
        if (index == active_) {
            state = Theme::ListRow::SELECTED;
        } else if (index == cursor_) {
            state = Theme::ListRow::CURSOR;
        }
        theme.draw_list_row(canvas, row_rect, state);
        if (font == nullptr) continue;
        int const y = row_rect.y + (h - font->height()) / 2;
        canvas.draw_text({row_rect.x + pad, y}, rows_[static_cast<size_t>(index)],
                         font, theme.color(ColorRole::TEXT));
    }
}

void ListView::on_mouse_down(const MouseEvent& event) {
    if (!enabled_ || event.button != MouseButton::LEFT) return;
    int const index = row_at(event.pos);
    if (index < 0) return;
    /* A click makes the row both the cursor and the selection. */
    set_cursor(index);
    select(index);
}

void ListView::on_mouse_move(const MouseEvent& event) {
    /* The cursor follows the pointer: the row pointed at is the dithered one
     * (specs/trinket/listview.md). */
    set_cursor(row_at(event.pos));
}

void ListView::on_mouse_leave(const MouseEvent&) {
    set_cursor(-1);
}

void ListView::on_key_down(const KeyEvent& event) {
    if (!enabled_ || !event.pressed || count() == 0) return;
    int const visible = visible_rows();
    int const page = visible > 0 ? visible : 1;
    int const from = active_ < 0 ? first_ : active_;
    int next = from;
    switch (event.code) {
        case KeyCode::UP: next = from - 1; break;
        case KeyCode::DOWN: next = from + 1; break;
        case KeyCode::PAGE_UP: next = from - page; break;
        case KeyCode::PAGE_DOWN: next = from + page; break;
        case KeyCode::HOME: next = 0; break;
        case KeyCode::END: next = count() - 1; break;
        default: return;
    }
    if (next < 0) next = 0;
    if (next >= count()) next = count() - 1;
    ensure_visible(next);
    select(next);
}

}  // namespace aegir::trinket
