/*
 * Trinket Scrollbar - a scrolling view's position (specs/trinket/scrollbar.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_SCROLLBAR_H
#define AEGIR_TRINKET_SCROLLBAR_H

#include <aegir/trinket/widget.h>
#include <functional>

namespace aegir::trinket {

class Canvas;

/* A scrollbar over content of `total` units, `page` of them visible. The value
 * is the first visible unit, in [0, total - page]; the thumb is the page, and
 * on_scroll reports a new value whether the thumb was dragged, the trough
 * clicked, or an arrow pressed. A view wires it to its own model -- the demo
 * gives it the terminal's scrollback. */
class Scrollbar : public Widget {
public:
    enum class Orientation { VERTICAL, HORIZONTAL };

    explicit Scrollbar(Orientation orientation = Orientation::VERTICAL);
    ~Scrollbar() override;

    Orientation orientation() const { return orientation_; }
    void set_orientation(Orientation o);

    void set_range(int total, int page);
    int total() const { return total_; }
    int page() const { return page_; }
    int maximum_value() const;

    void set_value(int value);
    int value() const { return value_; }

    /* The first visible unit changed by the user (a drag, a click, an arrow or
     * a key), not by set_value. */
    std::function<void(int)> on_scroll;

    bool focusable() const override { return true; }
    Size preferred_size() const override;
    Size minimum_size() const override;
    Size maximum_size() const override;

    /* The thumb's offset along the track and its length, and the value a thumb
     * dragged to a track offset means. Pure, so the host check asserts them
     * (specs/trinket/scrollbar.md). `track` is the run between the two arrows. */
    struct Thumb { int pos; int size; };
    static Thumb thumb_for(int track, int total, int page, int value, int min_handle);
    static int value_for_pos(int track, int total, int page, int pos, int min_handle);

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_mouse_down(const MouseEvent& event) override;
    void on_mouse_up(const MouseEvent& event) override;
    void on_mouse_move(const MouseEvent& event) override;
    void on_mouse_enter(const MouseEvent& event) override;
    void on_mouse_leave(const MouseEvent& event) override;
    void on_key_down(const KeyEvent& event) override;

private:
    /* The scrollbar's geometry: a raised frame, then the buttons at the foot,
     * and the trough and thumb set in from the frame -- so the thumb is
     * narrower than the bar (specs/trinket/scrollbar.md). The widget owns the
     * rectangles because the pointer handlers hit-test them; the theme only
     * draws what it is handed. */
    struct Parts {
        Rect trough;
        Rect thumb;
        Rect decrement;
        Rect increment;
    };
    Parts parts() const;

    int arrow() const;         // the arrow button size (the theme's)
    int min_handle() const;    // the least thumb length (the theme's)
    void scroll_to(int value);

    Orientation orientation_ = Orientation::VERTICAL;
    int total_ = 0;
    int page_ = 0;
    int value_ = 0;
    bool hovered_ = false;
    bool dragging_ = false;
    int grab_ = 0;  // where in the thumb the drag began
    /* The arrow held down: -1 decrement, +1 increment, 0 neither. The theme
     * draws its selected frame until the release (specs/trinket/scrollbar.md);
     * a pointer that leaves the bar releases it, because the toolkit has no
     * capture. */
    int pressed_ = 0;
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_SCROLLBAR_H
