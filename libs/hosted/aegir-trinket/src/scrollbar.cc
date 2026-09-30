/*
 * Trinket Scrollbar implementation (see the header for the shape).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/scrollbar.h>

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/theme.h>

#include <algorithm>
#include <limits>

namespace aegir::trinket {

Scrollbar::Scrollbar(Orientation orientation) : orientation_(orientation) {}

Scrollbar::~Scrollbar() = default;

void Scrollbar::set_orientation(Orientation o) {
    if (o == orientation_) return;
    orientation_ = o;
    damage();
}

int Scrollbar::maximum_value() const {
    return total_ > page_ ? total_ - page_ : 0;
}

void Scrollbar::set_range(int total, int page) {
    if (total < 0) total = 0;
    if (page < 0) page = 0;
    if (total == total_ && page == page_) return;
    total_ = total;
    page_ = page;
    int const max_value = maximum_value();
    if (value_ > max_value) value_ = max_value;
    damage();
}

void Scrollbar::set_value(int value) {
    int const max_value = maximum_value();
    if (value < 0) value = 0;
    if (value > max_value) value = max_value;
    if (value == value_) return;
    value_ = value;
    damage();
}

void Scrollbar::scroll_to(int value) {
    int const max_value = maximum_value();
    if (value < 0) value = 0;
    if (value > max_value) value = max_value;
    if (value == value_) return;
    value_ = value;
    damage();
    if (on_scroll) on_scroll(value_);
}

Scrollbar::Thumb Scrollbar::thumb_for(int track, int total, int page, int value,
                                      int min_handle) {
    Thumb thumb{0, 0};
    if (track <= 0) return thumb;
    if (total <= page || total <= 0) {
        /* Nothing to scroll: the thumb fills the track. */
        thumb.size = track;
        return thumb;
    }
    int const max_value = total - page;
    long long const scaled = static_cast<long long>(track) * page / total;
    int size = static_cast<int>(scaled);
    if (size < min_handle) size = min_handle;
    if (size > track) size = track;
    thumb.size = size;
    int const travel = track - size;
    int v = value < 0 ? 0 : (value > max_value ? max_value : value);
    thumb.pos =
        travel <= 0 ? 0 : static_cast<int>(static_cast<long long>(travel) * v / max_value);
    return thumb;
}

int Scrollbar::value_for_pos(int track, int total, int page, int pos, int min_handle) {
    if (total <= page || track <= 0) return 0;
    Thumb const thumb = thumb_for(track, total, page, 0, min_handle);
    int const travel = track - thumb.size;
    if (travel <= 0) return 0;
    int p = pos < 0 ? 0 : (pos > travel ? travel : pos);
    return static_cast<int>(static_cast<long long>(p) * (total - page) / travel);
}

int Scrollbar::along() const {
    return orientation_ == Orientation::VERTICAL ? rect_.height : rect_.width;
}

int Scrollbar::arrow() const {
    return Application::instance()->theme().metric(MetricRole::SCROLLBAR_ARROW_SIZE);
}

int Scrollbar::min_handle() const {
    return Application::instance()->theme().metric(MetricRole::SCROLLBAR_MIN_HANDLE);
}

int Scrollbar::track_length() const {
    int const track = along() - 2 * arrow();
    return track > 0 ? track : 0;
}

Size Scrollbar::preferred_size() const {
    int const width = Application::instance()->theme().metric(MetricRole::SCROLLBAR_WIDTH);
    int const along = 2 * arrow() + min_handle();
    return orientation_ == Orientation::VERTICAL ? Size{width, along} : Size{along, width};
}

Size Scrollbar::minimum_size() const {
    return preferred_size();
}

Size Scrollbar::maximum_size() const {
    int const width = Application::instance()->theme().metric(MetricRole::SCROLLBAR_WIDTH);
    int const big = std::numeric_limits<int>::max();
    return orientation_ == Orientation::VERTICAL ? Size{width, big} : Size{big, width};
}

void Scrollbar::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);
    Theme& theme = Application::instance()->theme();
    bool const vertical = orientation_ == Orientation::VERTICAL;
    Thumb const thumb = thumb_for(track_length(), total_, page_, value_, min_handle());
    theme.draw_scrollbar(canvas, rect_, vertical, arrow() + thumb.pos, thumb.size, hovered_);
}

void Scrollbar::on_mouse_enter(const MouseEvent&) {
    if (!hovered_) {
        hovered_ = true;
        damage();
    }
}

void Scrollbar::on_mouse_leave(const MouseEvent&) {
    if (hovered_) {
        hovered_ = false;
        damage();
    }
}

void Scrollbar::on_mouse_down(const MouseEvent& event) {
    if (event.button != MouseButton::LEFT) return;
    bool const vertical = orientation_ == Orientation::VERTICAL;
    int const origin = vertical ? rect_.y : rect_.x;
    int const local = (vertical ? event.pos.y : event.pos.x) - origin;
    int const length = along();
    if (local < 0 || local >= length) return;

    int const a = arrow();
    if (local < a) {
        scroll_to(value_ - 1);
        return;
    }
    if (local >= length - a) {
        scroll_to(value_ + 1);
        return;
    }

    int const track = track_length();
    int const in_track = local - a;
    Thumb const thumb = thumb_for(track, total_, page_, value_, min_handle());
    if (in_track >= thumb.pos && in_track < thumb.pos + thumb.size) {
        dragging_ = true;
        grab_ = in_track - thumb.pos;
        return;
    }
    /* A click in the trough pages toward the click. */
    int const step = page_ > 0 ? page_ : 1;
    scroll_to(in_track < thumb.pos ? value_ - step : value_ + step);
}

void Scrollbar::on_mouse_move(const MouseEvent& event) {
    if (!dragging_) return;
    bool const vertical = orientation_ == Orientation::VERTICAL;
    int const origin = vertical ? rect_.y : rect_.x;
    int const local = (vertical ? event.pos.y : event.pos.x) - origin;
    int const track = track_length();
    int const pos = local - arrow() - grab_;
    scroll_to(value_for_pos(track, total_, page_, pos, min_handle()));
}

void Scrollbar::on_mouse_up(const MouseEvent& event) {
    if (event.button == MouseButton::LEFT) dragging_ = false;
}

void Scrollbar::on_key_down(const KeyEvent& event) {
    if (!event.pressed) return;
    bool const vertical = orientation_ == Orientation::VERTICAL;
    KeyCode const forward = vertical ? KeyCode::DOWN : KeyCode::RIGHT;
    KeyCode const backward = vertical ? KeyCode::UP : KeyCode::LEFT;
    int const step = page_ > 0 ? page_ : 1;
    if (event.code == forward) {
        scroll_to(value_ + 1);
    } else if (event.code == backward) {
        scroll_to(value_ - 1);
    } else if (event.code == KeyCode::PAGE_DOWN) {
        scroll_to(value_ + step);
    } else if (event.code == KeyCode::PAGE_UP) {
        scroll_to(value_ - step);
    } else if (event.code == KeyCode::HOME) {
        scroll_to(0);
    } else if (event.code == KeyCode::END) {
        scroll_to(maximum_value());
    }
}

}  // namespace aegir::trinket
