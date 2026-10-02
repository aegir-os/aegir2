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

/* One cell's text, placed in its column by its alignment. Empty text draws
 * nothing, so a short row leaves the columns past it blank. */
void draw_cell(Canvas& canvas, Font* font, Theme& theme, Rect const& cell,
               std::u32string const& text, ListView::Alignment align) {
    if (font == nullptr || text.empty()) return;
    Size const size = font->measure(text);
    int x = cell.x;
    if (align == ListView::Alignment::CENTER) {
        x = cell.x + (cell.width - size.width) / 2;
    } else if (align == ListView::Alignment::RIGHT) {
        x = cell.x + cell.width - size.width;
    }
    int const y = cell.y + (cell.height - font->height()) / 2;
    canvas.draw_text({x, y}, text, font, theme.color(ColorRole::TEXT));
}
}  // namespace

ListView::ListView() = default;

ListView::~ListView() = default;

void ListView::on_layout() {
    /* The rectangle's share of the row height is what the list can show, and a
     * client that wires a scrollbar must re-sync it; report the first layout
     * and each change, but nothing else, so a layout that moved nothing does
     * not spin (specs/trinket/listview.md). */
    int const visible = visible_rows();
    if (visible == last_visible_) return;
    last_visible_ = visible;
    if (on_visible_changed) on_visible_changed();
}

void ListView::add(std::u32string_view text) {
    rows_.emplace_back();
    rows_.back().cells.emplace_back(text);
    damage();
}

void ListView::set_row(int index, std::vector<std::u32string> cells, Icon icon) {
    if (index < 0) return;
    if (index >= count()) {
        /* Past the last row appends, so a list may be built a row at a time. */
        rows_.resize(static_cast<size_t>(index) + 1);
    }
    rows_[static_cast<size_t>(index)].cells = std::move(cells);
    rows_[static_cast<size_t>(index)].icon = icon;
    if (icon != Icon::NONE) has_icons_ = true;
    damage();
}

void ListView::set_columns(std::vector<Column> columns) {
    columns_ = std::move(columns);
    damage();
}

ListView::Column const& ListView::column(int index) const {
    static const Column none;
    if (index < 0 || index >= column_count()) return none;
    return columns_[static_cast<size_t>(index)];
}

std::u32string const& ListView::cell(int index, int column) const {
    static const std::u32string none;
    if (index < 0 || index >= count()) return none;
    Row const& r = rows_[static_cast<size_t>(index)];
    if (column < 0 || column >= static_cast<int>(r.cells.size())) return none;
    return r.cells[static_cast<size_t>(column)];
}

const std::u32string& ListView::row(int index) const { return cell(index, 0); }

Icon ListView::row_icon(int index) const {
    if (index < 0 || index >= count()) return Icon::NONE;
    return rows_[static_cast<size_t>(index)].icon;
}

void ListView::clear() {
    rows_.clear();
    active_ = -1;
    cursor_ = -1;
    first_ = 0;
    has_icons_ = false;
    damage();
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

Rect ListView::data_rect() const {
    Rect const area = rows_rect();
    if (!titles()) return area;
    /* The titles row is the top row of the well. */
    int const titles = row_height();
    int const height = area.height - titles > 0 ? area.height - titles : 0;
    return {area.x, area.y + titles, area.width, height};
}

int ListView::visible_rows() const {
    int const h = row_height();
    int const height = data_rect().height;
    if (h <= 0 || height <= 0) return 0;
    return height / h;
}

int ListView::row_at(Point p) const {
    Rect const area = data_rect();
    int const h = row_height();
    if (h <= 0 || !area.contains(p)) return -1;
    int const index = first_ + (p.y - area.y) / h;
    return index < count() ? index : -1;
}

int ListView::height_for_rows(int rows) const {
    /* The well is the list's own chrome, and the titles row its own band: a
     * host that adds a frame adds that on top, and a host that counts rows for
     * one of these and forgets the well gets a list one row shorter than it
     * asked for -- which is how the menu first came up showing two of its three
     * entries. */
    int const titles_band = titles() ? row_height() : 0;
    return 2 * kWell + titles_band + std::max(0, rows) * row_height();
}

ListView::ColumnLayout ListView::column_layout(std::vector<Column> const& columns,
                                               int width, int padding, int gap) {
    ColumnLayout out;
    if (columns.empty()) return out;
    if (padding < 0) padding = 0;
    /* A gap between columns: a right-aligned cell ends at its column's right
     * edge and the next column's left-aligned cell begins at that same edge, so
     * with no gap the two touch -- a size against the date beside it
     * (specs/trinket/listview.md). It is the caller's, the theme's spacing on
     * the target, so the arithmetic stays pure and the host check pins it. */
    if (gap < 0) gap = 0;
    int const gaps = (static_cast<int>(columns.size()) - 1) * gap;
    int const content = width - 2 * padding - gaps;
    int fixed = 0;
    int free_columns = 0;
    for (Column const& c : columns) {
        if (c.width > 0) {
            fixed += c.width;
        } else {
            ++free_columns;
        }
    }
    int const free = content > fixed ? content - fixed : 0;
    int const share = free_columns > 0 ? free / free_columns : 0;
    int x = padding;
    for (Column const& c : columns) {
        int const w = c.width > 0 ? c.width : share;
        out.x.push_back(x);
        out.width.push_back(w);
        x += w + gap;
    }
    return out;
}

Size ListView::preferred_size() const {
    Theme& theme = Application::instance()->theme();
    int const rows = std::max(1, theme.metric(MetricRole::LIST_PREFERRED_ROWS));
    return {theme.metric(MetricRole::LIST_MIN_WIDTH), height_for_rows(rows)};
}

Size ListView::minimum_size() const {
    /* One row and no wider than it must be: a list in a popup is given the
     * popup's height, and one that insists on its preferred -- four rows -- is
     * laid out taller than the frame it is in and paints over the frame's foot
     * (specs/trinket/listview.md). A list that wants its preferred says so with
     * its weight, which a host may leave at its default. */
    return {Application::instance()->theme().metric(MetricRole::LIST_MIN_WIDTH),
            height_for_rows(1)};
}

Size ListView::maximum_size() const {
    return {std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
}

void ListView::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);
    Theme& theme = Application::instance()->theme();
    Font* const font = Application::instance()->default_font();
    theme.draw_list(canvas, rect_);
    int const h = row_height();
    if (h <= 0) return;
    int const pad = theme.metric(MetricRole::LIST_PADDING_H);
    /* The leading strip an image sits in, so the columns start after it. A list
     * with no image reserves nothing. */
    int const icon_size = theme.metric(MetricRole::ICON_SIZE_SMALL);
    int const gap = theme.metric(MetricRole::SPACING_SMALL);
    int const strip = has_icons_ && !columns_.empty() ? icon_size + gap : 0;
    Rect const area = rows_rect();
    Rect const data = data_rect();

    /* The titles row, when there is a column table: the columns' own layout,
     * with no image and no row state. */
    if (titles()) {
        Rect const titles_band{area.x, area.y, area.width, h};
        ColumnLayout const layout = column_layout(columns_, titles_band.width - strip, pad, gap);
        for (int c = 0; c < column_count() && c < static_cast<int>(layout.x.size()); ++c) {
            Rect const cell_rect{titles_band.x + strip + layout.x[c], titles_band.y,
                                 layout.width[c], h};
            draw_cell(canvas, font, theme, cell_rect, columns_[static_cast<size_t>(c)].title,
                      columns_[static_cast<size_t>(c)].align);
        }
        /* A single rule under the titles, so the header reads apart from the
         * rows (specs/trinket/listview.md). */
        canvas.draw_hline(area.x, area.x + area.width - 1, titles_band.y + h - 1,
                          theme.color(ColorRole::BORDER_DARK));
    }

    ColumnLayout const layout =
        columns_.empty() ? ColumnLayout{} : column_layout(columns_, data.width - strip, pad, gap);
    int const visible = visible_rows();
    for (int i = 0; i < visible; ++i) {
        int const index = first_ + i;
        if (index >= count()) break;
        Rect const row_rect{data.x, data.y + i * h, data.width, h};
        Theme::ListRow state = Theme::ListRow::NORMAL;
        if (index == active_) {
            state = Theme::ListRow::SELECTED;
        } else if (index == cursor_) {
            state = Theme::ListRow::CURSOR;
        }
        theme.draw_list_row(canvas, row_rect, state);
        if (font == nullptr) continue;
        Row const& r = rows_[static_cast<size_t>(index)];
        if (strip > 0 && r.icon != Icon::NONE) {
            Rect const image{row_rect.x + pad, row_rect.y + (h - icon_size) / 2, icon_size,
                             icon_size};
            theme.draw_icon(canvas, image, r.icon);
        }
        if (columns_.empty()) {
            /* The simple shape: one text column, the widget's own alignment. */
            Rect const cell_rect{row_rect.x + pad, row_rect.y, row_rect.width - 2 * pad, h};
            draw_cell(canvas, font, theme, cell_rect, row(index), align_);
            continue;
        }
        for (int c = 0; c < column_count() && c < static_cast<int>(layout.x.size()); ++c) {
            Rect const cell_rect{row_rect.x + strip + layout.x[c], row_rect.y,
                                 layout.width[c], h};
            draw_cell(canvas, font, theme, cell_rect, cell(index, c),
                      columns_[static_cast<size_t>(c)].align);
        }
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

}  // namespace aegir::trinket
