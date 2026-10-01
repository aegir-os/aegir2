/*
 * Trinket ListView - a column of rows, one of them chosen
 * (specs/trinket/listview.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_LISTVIEW_H
#define AEGIR_TRINKET_LISTVIEW_H

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
 * not own one (specs/trinket/listview.md). */
class ListView : public Widget {
public:
    ListView();
    ~ListView() override;

    void add(std::u32string_view text);
    void clear();
    int count() const { return static_cast<int>(rows_.size()); }
    const std::u32string& row(int index) const;

    void set_active(int index);  // programmatic: no on_select; -1 for none
    int active() const { return active_; }
    void set_cursor(int index);  // -1 for none
    int cursor() const { return cursor_; }

    void set_first(int index);
    int first() const { return first_; }
    int visible_rows() const;

    /* The user chose a row (a click or a key), not set_active. */
    std::function<void(int)> on_select;

    /* The row's height -- the font's line and its pad -- and the row a point in
     * the widget is over, or -1. */
    int row_height() const;
    int row_at(Point p) const;

    /* The first row a scroll to `value` wants, clamped to `[0, count -
     * visible]`. Pure, so the host check pins it (specs/trinket/listview.md). */
    static int clamp_first(int value, int count, int visible);

    bool focusable() const override { return true; }
    Size preferred_size() const override;
    Size minimum_size() const override;
    Size maximum_size() const override;

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_mouse_down(const MouseEvent& event) override;
    void on_mouse_move(const MouseEvent& event) override;
    void on_mouse_leave(const MouseEvent& event) override;
    void on_key_down(const KeyEvent& event) override;

private:
    /* The rows' area: inside the well's one-pixel outline. */
    Rect rows_rect() const;
    void select(int index);  // the user's choice
    void ensure_visible(int index);

    std::vector<std::u32string> rows_;
    int active_ = -1;
    int cursor_ = -1;
    int first_ = 0;
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_LISTVIEW_H
