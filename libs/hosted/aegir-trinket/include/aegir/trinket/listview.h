/*
 * Trinket ListView - a column of rows, one of them chosen
 * (specs/trinket/listview.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_LISTVIEW_H
#define AEGIR_TRINKET_LISTVIEW_H

#include <aegir/trinket/icon.h>
#include <aegir/trinket/widget.h>
#include <functional>
#include <string>
#include <vector>

namespace aegir::trinket {

class Canvas;

/* A list of rows with one active row and a cursor on the row the pointer points
 * at. A click selects the row under it and reports it; the arrows move the
 * selection a row, Page Up/Down a page and Home/End to the ends, scrolling the
 * list to keep it in view. `first` is the top visible row -- the `Scrollbar` is
 * the control for it, as it is for the terminal's scrollback, so the list does
 * not own one (specs/trinket/listview.md).
 *
 * A row is one text cell by default (`add`). A *column table* (`set_columns`)
 * gives it titled columns and per-column cells, with an optional image before
 * the first cell -- the shape a file requester's list is
 * (specs/trinket/file_requester.md). */
class ListView : public Widget {
public:
    /* Where a cell's text sits in its column: a file list is left, a menu is
     * centred (specs/trinket/listview.md). */
    enum class Alignment { LEFT, CENTER, RIGHT };

    /* One column: a title, a width in pixels, and where its cells sit. A width
     * of 0 takes an equal share of the width the fixed columns leave, so the
     * free column is the one that grows. */
    struct Column {
        std::u32string title;
        int width = 0;
        Alignment align = Alignment::LEFT;
    };

    ListView();
    ~ListView() override;

    void set_align(Alignment a) { align_ = a; damage(); }
    Alignment align() const { return align_; }

    /* The simple shape: one text column, no titles row. */
    void add(std::u32string_view text);
    const std::u32string& row(int index) const;

    /* The columned shape: a titles row inside the well, and per-column cells.
     * Setting a table leaves the rows already added as the first column's
     * cells. */
    void set_columns(std::vector<Column> columns);
    int column_count() const { return static_cast<int>(columns_.size()); }
    Column const& column(int index) const;

    /* One row's cells (one per column, short of them is empty) and its image.
     * `index` past the last row appends, so a list may be built a row at a
     * time. */
    void set_row(int index, std::vector<std::u32string> cells, Icon icon = Icon::NONE);
    std::u32string const& cell(int index, int column) const;
    Icon row_icon(int index) const;

    void clear();
    int count() const { return static_cast<int>(rows_.size()); }

    void set_active(int index);  // programmatic: no on_select; -1 for none
    int active() const { return active_; }
    void set_cursor(int index);  // -1 for none
    int cursor() const { return cursor_; }

    void set_first(int index);
    int first() const { return first_; }
    int visible_rows() const;

    /* The user chose a row (a click or a key), not set_active. */
    std::function<void(int)> on_select;

    /* The rectangle changed what the list can show, and so `visible_rows`, so a
     * client wiring a scrollbar re-syncs it. Reported only on a change, so a
     * layout that moved nothing does not spin (specs/trinket/listview.md). */
    std::function<void()> on_visible_changed;

    /* The row's height -- the font's line and its pad -- and the row a point in
     * the widget is over, or -1. */
    int row_height() const;
    /* The height that shows `rows` of them whole: the well, the titles row when
     * there is one, and the rows. A host that frames a list -- a popup does --
     * adds its own inset to this (specs/trinket/listview.md). */
    int height_for_rows(int rows) const;
    int row_at(Point p) const;

    /* The first row a scroll to `value` wants, clamped to `[0, count -
     * visible]`. Pure, so the host check pins it (specs/trinket/listview.md). */
    static int clamp_first(int value, int count, int visible);

    /* The columns' x and width across a row `width` wide, `padding` in at either
     * end and `gap` between. Pure -- no font, no theme -- so the host check
     * pins it; the caller passes the theme's spacing as `gap`. */
    struct ColumnLayout {
        std::vector<int> x;
        std::vector<int> width;
    };
    static ColumnLayout column_layout(std::vector<Column> const& columns, int width,
                                      int padding, int gap);

    bool focusable() const override { return true; }
    Size preferred_size() const override;
    Size minimum_size() const override;
    Size maximum_size() const override;

    /* Report each visible row's screen rectangle as a rect cue,
     * `prefix.row.<n>` with n the one-based row index (specs/testing.md), so
     * the acceptance clicks a row by name; a row's height is the font's, so a
     * fixed fraction of the list would not name the same row after a font
     * change. */
    void report_parts(char const *prefix) const override;

protected:
    void on_layout() override;
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_mouse_down(const MouseEvent& event) override;
    void on_mouse_move(const MouseEvent& event) override;
    void on_mouse_leave(const MouseEvent& event) override;
    void on_key_down(const KeyEvent& event) override;

private:
    /* The well's inside, and the data rows' part of it (the titles row, when
     * there is one, is the top). */
    Rect rows_rect() const;
    Rect data_rect() const;
    void select(int index);  // the user's choice
    void ensure_visible(int index);
    /* Whether the titles row is drawn: a columned list has one. */
    bool titles() const { return !columns_.empty(); }

    struct Row {
        std::vector<std::u32string> cells;
        Icon icon = Icon::NONE;
    };

    std::vector<Row> rows_;
    std::vector<Column> columns_;
    /* Whether any row carries an image: the leading strip the columns start
     * after. Set by set_row, cleared by clear. */
    bool has_icons_ = false;
    Alignment align_ = Alignment::LEFT;
    int active_ = -1;
    int cursor_ = -1;
    /* The last rectangle's share, so `on_visible_changed` fires on a change. */
    int last_visible_ = -1;
    int first_ = 0;
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_LISTVIEW_H
