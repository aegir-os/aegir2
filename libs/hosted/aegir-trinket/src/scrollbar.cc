/*
 * Trinket Scrollbar implementation (see the header for the shape).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/scrollbar.h>

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/diagnostics.h>
#include <aegir/trinket/theme.h>

#include <algorithm>
#include <limits>

namespace aegir::trinket {

namespace {
/* The trough container's frame, and the well the dither and thumb are set in
 * from it. The buttons below are separate cells, not inside this frame
 * (specs/trinket/scrollbar.md). */
constexpr int kWell = 4;
}  // namespace

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

int Scrollbar::arrow() const {
    return Application::instance()->theme().metric(MetricRole::SCROLLBAR_ARROW_SIZE);
}

int Scrollbar::min_handle() const {
    return Application::instance()->theme().metric(MetricRole::SCROLLBAR_MIN_HANDLE);
}

Scrollbar::Parts Scrollbar::parts() const {
    bool const vertical = orientation_ == Orientation::VERTICAL;
    int const arrow = this->arrow();
    int const buttons = 2 * arrow;
    Parts parts;
    /* The trough is the whole strip above the buttons, and the buttons are
     * separate cells below it, the same width -- the XEN scrollbar has no frame
     * around the two together (specs/trinket/scrollbar.md). */
    if (vertical) {
        parts.decrement =
            Rect{rect_.x, rect_.y + rect_.height - buttons, rect_.width, arrow};
        parts.increment = Rect{rect_.x, rect_.y + rect_.height - arrow, rect_.width, arrow};
        parts.trough = Rect{rect_.x, rect_.y, rect_.width, rect_.height - buttons};
    } else {
        parts.decrement =
            Rect{rect_.x + rect_.width - buttons, rect_.y, arrow, rect_.height};
        parts.increment = Rect{rect_.x + rect_.width - arrow, rect_.y, arrow, rect_.height};
        parts.trough = Rect{rect_.x, rect_.y, rect_.width - buttons, rect_.height};
    }
    /* The thumb rides in the trough's well, set in from its container frame. */
    Rect const well = parts.trough.inflated(-kWell);
    int const track = vertical ? well.height : well.width;
    Thumb const thumb = thumb_for(track, total_, page_, value_, min_handle());
    parts.thumb = vertical ? Rect{well.x, well.y + thumb.pos, well.width, thumb.size}
                           : Rect{well.x + thumb.pos, well.y, thumb.size, well.height};
    return parts;
}

void Scrollbar::report_parts(char const *prefix) const {
    Parts const p = parts();
    report_rect_prefix(prefix, "decrement", screen_rect_of(*this, p.decrement));
    report_rect_prefix(prefix, "increment", screen_rect_of(*this, p.increment));
    report_rect_prefix(prefix, "trough", screen_rect_of(*this, p.trough));
    report_rect_prefix(prefix, "thumb", screen_rect_of(*this, p.thumb));
}

Size Scrollbar::preferred_size() const {
    int const width = Application::instance()->theme().metric(MetricRole::SCROLLBAR_WIDTH);
    int const along = 2 * kWell + 2 * arrow() + min_handle();
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
    Parts const p = parts();
    theme.draw_scrollbar(canvas, rect_, orientation_ == Orientation::VERTICAL, p.trough,
                         p.thumb, p.decrement, p.increment, pressed_ == -1, pressed_ == 1);
}

void Scrollbar::on_mouse_enter(const MouseEvent&) {
    if (!hovered_) {
        hovered_ = true;
        damage();
    }
}

void Scrollbar::on_mouse_leave(const MouseEvent&) {
    /* No capture: a pointer that leaves releases the arrow it was holding. */
    bool const changed = hovered_ || pressed_ != 0;
    hovered_ = false;
    pressed_ = 0;
    if (changed) damage();
}

void Scrollbar::on_mouse_down(const MouseEvent& event) {
    if (event.button != MouseButton::LEFT) return;
    bool const vertical = orientation_ == Orientation::VERTICAL;
    Parts const p = parts();

    /* The buttons are at the far end of the run: the increment is the last cell
     * and the decrement the one before it -- below a vertical bar, right of a
     * horizontal one (specs/trinket/scrollbar.md). */
    if (p.decrement.contains(event.pos)) {
        if (pressed_ != -1) {
            pressed_ = -1;
            damage();
        }
        scroll_to(value_ - 1);
        return;
    }
    if (p.increment.contains(event.pos)) {
        if (pressed_ != 1) {
            pressed_ = 1;
            damage();
        }
        scroll_to(value_ + 1);
        return;
    }
    /* The thumb is grabbed where it was pressed, so it does not jump under the
     * pointer; a click elsewhere in the narrow trough pages toward it. */
    if (p.thumb.contains(event.pos)) {
        dragging_ = true;
        grab_ = vertical ? event.pos.y - p.thumb.y : event.pos.x - p.thumb.x;
        return;
    }
    if (!p.trough.contains(event.pos)) return;
    int const step = page_ > 0 ? page_ : 1;
    bool const before = vertical ? event.pos.y < p.thumb.y : event.pos.x < p.thumb.x;
    scroll_to(before ? value_ - step : value_ + step);
}

void Scrollbar::on_mouse_move(const MouseEvent& event) {
    if (!dragging_) return;
    bool const vertical = orientation_ == Orientation::VERTICAL;
    Parts const p = parts();
    int const track = vertical ? p.trough.height : p.trough.width;
    int const local = vertical ? event.pos.y - p.trough.y : event.pos.x - p.trough.x;
    int const pos = local - grab_;
    scroll_to(value_for_pos(track, total_, page_, pos, min_handle()));
}

void Scrollbar::on_mouse_up(const MouseEvent& event) {
    if (event.button != MouseButton::LEFT) return;
    dragging_ = false;
    if (pressed_ != 0) {
        pressed_ = 0;
        damage();
    }
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
