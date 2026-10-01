/*
 * Trinket Slider - a value chosen by dragging a knob (specs/trinket/slider.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_SLIDER_H
#define AEGIR_TRINKET_SLIDER_H

#include <aegir/trinket/widget.h>
#include <functional>

namespace aegir::trinket {

class Canvas;

/* A slider over the range `[min, max]`: the knob's offset along the track is the
 * value's share of the travel, and `on_change` reports a value the user set --
 * by dragging the knob, clicking the trough, or a key. A vertical slider maps
 * the same numbers down the track; a horizontal one is the default. The trough
 * and the knob are drawn by the theme, which shares the XEN look with the
 * scrollbar (specs/trinket/slider.md). */
class Slider : public Widget {
public:
    enum class Orientation { HORIZONTAL, VERTICAL };

    explicit Slider(Orientation orientation = Orientation::HORIZONTAL);
    ~Slider() override;

    Orientation orientation() const { return orientation_; }
    void set_orientation(Orientation o);

    void set_range(int min, int max);
    int minimum() const { return min_; }
    int maximum() const { return max_; }

    void set_value(int value);
    int value() const { return value_; }

    /* How far a trough click or an arrow key moves the value. */
    void set_step(int step);
    int step() const { return step_; }

    /* The value changed by the user (a drag, a click or a key), not by
     * set_value. */
    std::function<void(int)> on_change;

    bool focusable() const override { return true; }
    Size preferred_size() const override;
    Size minimum_size() const override;
    Size maximum_size() const override;

    /* The knob's offset and length along the track, and the value a knob
     * dragged to a track offset means. Pure, so the host check pins them
     * (specs/trinket/slider.md). `track` is the run inside the trough's well. */
    struct Knob { int pos; int size; };
    static Knob knob_for(int track, int min, int max, int value, int knob);
    static int value_for_pos(int track, int min, int max, int pos, int knob);

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_mouse_down(const MouseEvent& event) override;
    void on_mouse_up(const MouseEvent& event) override;
    void on_mouse_move(const MouseEvent& event) override;
    void on_mouse_enter(const MouseEvent& event) override;
    void on_mouse_leave(const MouseEvent& event) override;
    void on_key_down(const KeyEvent& event) override;

private:
    /* The trough and the knob's rectangles: the widget owns them because the
     * pointer handlers hit-test them; the theme only draws what it is handed
     * (specs/trinket/slider.md). */
    struct Parts {
        Rect trough;
        Rect knob;
    };
    Parts parts() const;

    int thickness() const;    // the cross size (the theme's)
    int knob_length() const;  // the knob along the track (the theme's)
    void set_value_from_user(int value);

    Orientation orientation_ = Orientation::HORIZONTAL;
    int min_ = 0;
    int max_ = 0;
    int value_ = 0;
    int step_ = 1;
    bool hovered_ = false;
    bool dragging_ = false;
    int grab_ = 0;  // where in the knob the drag began
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_SLIDER_H
