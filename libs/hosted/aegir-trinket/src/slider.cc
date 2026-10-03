/*
 * Trinket Slider implementation (see the header for the shape).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/slider.h>

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/diagnostics.h>
#include <aegir/trinket/theme.h>

#include <limits>

namespace aegir::trinket {

namespace {
/* The trough's frame, and the well the dither and the knob are set in from it
 * -- the same four pixels the scrollbar uses, so the two share their look
 * (specs/trinket/slider.md). */
constexpr int kWell = 4;
}  // namespace

Slider::Slider(Orientation orientation) : orientation_(orientation) {}

Slider::~Slider() = default;

void Slider::set_orientation(Orientation o) {
    if (o == orientation_) return;
    orientation_ = o;
    damage();
}

void Slider::set_range(int min, int max) {
    if (max < min) max = min;
    if (min == min_ && max == max_) return;
    min_ = min;
    max_ = max;
    if (value_ < min_) value_ = min_;
    if (value_ > max_) value_ = max_;
    damage();
}

void Slider::set_value(int value) {
    if (value < min_) value = min_;
    if (value > max_) value = max_;
    if (value == value_) return;
    value_ = value;
    damage();
}

void Slider::set_step(int step) {
    if (step < 1) step = 1;
    step_ = step;
}

void Slider::set_value_from_user(int value) {
    if (value < min_) value = min_;
    if (value > max_) value = max_;
    if (value == value_) return;
    value_ = value;
    damage();
    if (on_change) on_change(value_);
}

Slider::Knob Slider::knob_for(int track, int min, int max, int value, int knob) {
    Knob k{0, 0};
    if (track <= 0) return k;
    int size = knob < 0 ? 0 : knob;
    if (size > track) size = track;
    k.size = size;
    int const travel = track - size;
    if (travel <= 0 || max <= min) return k;
    int v = value < min ? min : (value > max ? max : value);
    k.pos = static_cast<int>(static_cast<long long>(travel) * (v - min) / (max - min));
    return k;
}

int Slider::value_for_pos(int track, int min, int max, int pos, int knob) {
    if (max <= min || track <= 0) return min;
    int size = knob < 0 ? 0 : knob;
    if (size > track) size = track;
    int const travel = track - size;
    if (travel <= 0) return min;
    int p = pos < 0 ? 0 : (pos > travel ? travel : pos);
    return min + static_cast<int>(static_cast<long long>(p) * (max - min) / travel);
}

int Slider::thickness() const {
    return Application::instance()->theme().metric(MetricRole::SLIDER_THICKNESS);
}

int Slider::knob_length() const {
    return Application::instance()->theme().metric(MetricRole::SLIDER_KNOB_LENGTH);
}

Slider::Parts Slider::parts() const {
    bool const horizontal = orientation_ == Orientation::HORIZONTAL;
    Parts parts;
    /* The trough is the whole strip; the knob rides in its well, set in from the
     * container frame (specs/trinket/slider.md). */
    parts.trough = rect_;
    Rect const well = rect_.inflated(-kWell);
    int const track = horizontal ? well.width : well.height;
    Knob const knob = knob_for(track, min_, max_, value_, knob_length());
    parts.knob = horizontal
                     ? Rect{well.x + knob.pos, well.y, knob.size, well.height}
                     : Rect{well.x, well.y + knob.pos, well.width, knob.size};
    return parts;
}

void Slider::report_parts(char const *prefix) const {
    Parts const p = parts();
    report_rect_prefix(prefix, "trough", screen_rect_of(*this, p.trough));
    report_rect_prefix(prefix, "knob", screen_rect_of(*this, p.knob));
}

Size Slider::preferred_size() const {
    int const across = thickness();
    int const along = Application::instance()->theme().metric(MetricRole::SLIDER_MIN_LENGTH);
    return orientation_ == Orientation::HORIZONTAL ? Size{along, across} : Size{across, along};
}

Size Slider::minimum_size() const {
    int const across = thickness();
    int const along = 2 * kWell + knob_length();
    return orientation_ == Orientation::HORIZONTAL ? Size{along, across} : Size{across, along};
}

Size Slider::maximum_size() const {
    int const across = thickness();
    int const big = std::numeric_limits<int>::max();
    return orientation_ == Orientation::HORIZONTAL ? Size{big, across} : Size{across, big};
}

void Slider::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);
    Theme& theme = Application::instance()->theme();
    Parts const p = parts();
    theme.draw_slider(canvas, p.trough, p.knob, hovered_);
}

void Slider::on_mouse_enter(const MouseEvent&) {
    if (!hovered_) {
        hovered_ = true;
        damage();
    }
}

void Slider::on_mouse_leave(const MouseEvent&) {
    if (hovered_) {
        hovered_ = false;
        damage();
    }
}

void Slider::on_mouse_down(const MouseEvent& event) {
    if (event.button != MouseButton::LEFT) return;
    bool const horizontal = orientation_ == Orientation::HORIZONTAL;
    Parts const p = parts();

    /* The knob is grabbed where it was pressed, so it does not jump under the
     * pointer; a click elsewhere in the trough steps toward it. */
    if (p.knob.contains(event.pos)) {
        dragging_ = true;
        grab_ = horizontal ? event.pos.x - p.knob.x : event.pos.y - p.knob.y;
        return;
    }
    if (!p.trough.contains(event.pos)) return;
    bool const before = horizontal ? event.pos.x < p.knob.x : event.pos.y < p.knob.y;
    set_value_from_user(before ? value_ - step_ : value_ + step_);
}

void Slider::on_mouse_move(const MouseEvent& event) {
    if (!dragging_) return;
    bool const horizontal = orientation_ == Orientation::HORIZONTAL;
    Rect const well = rect_.inflated(-kWell);
    int const track = horizontal ? well.width : well.height;
    int const local = horizontal ? event.pos.x - well.x : event.pos.y - well.y;
    int const pos = local - grab_;
    set_value_from_user(value_for_pos(track, min_, max_, pos, knob_length()));
}

void Slider::on_mouse_up(const MouseEvent& event) {
    if (event.button == MouseButton::LEFT) dragging_ = false;
}

void Slider::on_key_down(const KeyEvent& event) {
    if (!event.pressed) return;
    bool const horizontal = orientation_ == Orientation::HORIZONTAL;
    KeyCode const forward = horizontal ? KeyCode::RIGHT : KeyCode::DOWN;
    KeyCode const backward = horizontal ? KeyCode::LEFT : KeyCode::UP;
    if (event.code == forward) {
        set_value_from_user(value_ + step_);
    } else if (event.code == backward) {
        set_value_from_user(value_ - step_);
    } else if (event.code == KeyCode::HOME) {
        set_value_from_user(min_);
    } else if (event.code == KeyCode::END) {
        set_value_from_user(max_);
    }
}

}  // namespace aegir::trinket
