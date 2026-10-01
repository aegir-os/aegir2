/*
 * Trinket Cycle implementation (see the header for the shape).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/cycle.h>

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/group.h>
#include <aegir/trinket/listview.h>
#include <aegir/trinket/popup.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/window.h>

#include <algorithm>
#include <limits>

namespace aegir::trinket {

Cycle::Cycle(std::vector<std::u32string> entries) : entries_(std::move(entries)) {}

Cycle::~Cycle() = default;

void Cycle::set_entries(std::vector<std::u32string> entries) {
    entries_ = std::move(entries);
    if (active_ >= static_cast<int>(entries_.size())) {
        active_ = entries_.empty() ? 0 : static_cast<int>(entries_.size()) - 1;
    }
    damage();
}

void Cycle::add(std::u32string_view entry) {
    entries_.emplace_back(entry);
    damage();
}

const std::u32string& Cycle::entry(int index) const {
    static const std::u32string none;
    if (index < 0 || index >= static_cast<int>(entries_.size())) return none;
    return entries_[static_cast<size_t>(index)];
}

const std::u32string& Cycle::active_entry() const { return entry(active_); }

void Cycle::set_active(int index) {
    int const n = static_cast<int>(entries_.size());
    if (n == 0) {
        active_ = 0;
        return;
    }
    if (index < 0) index = 0;
    if (index >= n) index = n - 1;
    if (index == active_) return;
    active_ = index;
    damage();
}

int Cycle::step_index(int active, int count, int delta, bool wrap) {
    if (count <= 0) return 0;
    int next = active + delta;
    if (wrap) {
        next %= count;
        if (next < 0) next += count;
    } else {
        if (next < 0) next = 0;
        if (next >= count) next = count - 1;
    }
    return next;
}

void Cycle::go_to(int index) {
    int const n = static_cast<int>(entries_.size());
    if (n == 0) return;
    if (index < 0) index = 0;
    if (index >= n) index = n - 1;
    if (index == active_) return;
    active_ = index;
    damage();
    if (on_changed) on_changed(active_);
}

void Cycle::step(int delta) {
    go_to(step_index(active_, static_cast<int>(entries_.size()), delta, wrap_));
}

Rect Cycle::divider_rect() const {
    /* One pixel, `CYCLE_BUTTON_WIDTH` in from the left edge, inside the box's
     * one-pixel frame (specs/trinket/cycle.md). */
    Theme& theme = Application::instance()->theme();
    int const cell = theme.metric(MetricRole::CYCLE_BUTTON_WIDTH);
    int const inner = std::max(0, rect_.width - 2);
    return Rect{rect_.x + 1 + std::min(cell, inner), rect_.y + 1, 1,
                std::max(0, rect_.height - 2)};
}

Rect Cycle::mark_rect() const {
    /* A small square centred in the cell left of the divider. */
    Theme& theme = Application::instance()->theme();
    int const side = theme.metric(MetricRole::CYCLE_MARK_SIZE);
    int const cell_x = rect_.x + 1;
    int const cell_w = std::max(0, divider_rect().x - cell_x);
    int const cell_h = std::max(0, rect_.height - 2);
    int const s = std::min(side, std::min(cell_w, cell_h));
    return Rect{cell_x + (cell_w - s) / 2, rect_.y + 1 + (cell_h - s) / 2, s, s};
}

Size Cycle::preferred_size() const {
    Theme& theme = Application::instance()->theme();
    Font* const font = Application::instance()->default_font();
    int text = 0;
    if (font != nullptr) {
        for (std::u32string const& e : entries_) {
            text = std::max(text, font->measure(e).width);
        }
    }
    int const cell = theme.metric(MetricRole::CYCLE_BUTTON_WIDTH);
    int const pad = 2 * theme.metric(MetricRole::BUTTON_PADDING_H);
    int const min_h = theme.metric(MetricRole::BUTTON_MIN_HEIGHT);
    int const text_h =
        font != nullptr ? font->height() + 2 * theme.metric(MetricRole::SPACING_SMALL) : 0;
    /* The box's two-pixel frame, the button cell, and the text with a pad each
     * side (specs/trinket/cycle.md). */
    return {2 + cell + pad + text + pad, std::max(min_h, text_h)};
}

Size Cycle::minimum_size() const { return preferred_size(); }

Size Cycle::maximum_size() const {
    return {std::numeric_limits<int>::max(), preferred_size().height};
}

void Cycle::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);
    Theme& theme = Application::instance()->theme();
    /* The box, the divider and the mark are the theme's; the entry's text is
     * the widget's, as a Button's label is (specs/trinket/cycle.md). */
    Rect const divider = divider_rect();
    theme.draw_cycle(canvas, rect_, divider, mark_rect(), pressed_, hovered_);
    Font* const font = Application::instance()->default_font();
    if (font == nullptr || entries_.empty()) return;
    std::u32string const& text = entries_[static_cast<size_t>(active_)];
    Size const size = font->measure(text);
    /* Centred between the divider and the box's right edge. */
    int const left = divider.x + 1;
    int const area = std::max(0, rect_.x + rect_.width - 1 - left);
    int const x = left + (area - size.width) / 2;
    int const y = rect_.y + (rect_.height - font->height()) / 2;
    canvas.draw_text({x, y}, text, font, theme.color(ColorRole::TEXT));
}

void Cycle::on_mouse_down(const MouseEvent& event) {
    if (!enabled_) return;
    if (event.button == MouseButton::LEFT) {
        pressed_ = true;
        damage();
    }
}

void Cycle::on_mouse_up(const MouseEvent& event) {
    if (!enabled_) return;
    if (!pressed_ || event.button != MouseButton::LEFT) return;
    pressed_ = false;
    damage();
    /* MUI's split, now that there is a popup (specs/trinket/cycle.md): a click
     * on the button cell -- and a Shift+click anywhere -- moves the entry, and
     * a click on the text opens the menu of them. */
    bool const back = (event.modifiers & kModShift) != 0;
    if (!back && event.pos.x >= divider_rect().x) {
        open_menu();
        return;
    }
    step(back ? -1 : 1);
}

/* The entries, as a menu under the gadget: a list with the active entry on it,
 * anchored below the cycle (above it when the window has no room below), and
 * framed in the outline the MUI screenshot shows (specs/trinket/popup.md). */
void Cycle::open_menu() {
    Window* const win = window();
    if (win == nullptr || entries_.empty()) return;

    Theme& theme = Application::instance()->theme();
    Font* const font = Application::instance()->default_font();
    int const inset = Group::frame_inset(Group::Frame::FRAME);
    int const pad = theme.metric(MetricRole::LIST_PADDING_H);

    auto list = std::make_unique<ListView>();
    int text = 0;
    for (std::u32string const& entry : entries_) {
        list->add(entry);
        if (font != nullptr) text = std::max(text, font->measure(entry).width);
    }
    list->set_active(active_);
    /* The list's own height for its entries, plus the frame's inset: counting
     * the rows without the list's well asks for one row less than it needs. */
    int const height = list->height_for_rows(count()) + 2 * inset;
    /* A pick is the cycle's change to make, so it goes through the same door
     * the keys do; the menu is the window's to close. */
    list->on_select = [this, win](int index) {
        win->close_popup();
        go_to(index);
    };

    auto frame = std::make_unique<Group>(Group::Orientation::VERTICAL, 0);
    frame->set_frame(Group::Frame::FRAME);
    ListView* const list_ptr = list.get();
    frame->add_child(std::move(list));
    /* The list takes the popup's height, so a menu of three entries is three
     * rows and not the list's preferred four (specs/trinket/listview.md). */
    frame->set_weight(list_ptr, 1);

    int const width = std::max(theme.metric(MetricRole::LIST_MIN_WIDTH), text + 2 * pad) +
                      2 * inset;
    /* The menu hangs under the entry's text, just past the button cell -- not
     * from the box's left edge, which would put it under the mark
     * (specs/trinket/cycle.md). Its height is the box's, so `popup_rect` places
     * it at the box's foot. */
    Rect const anchor{divider_rect().x + 1, rect_.y,
                      std::max(0, rect_.x + rect_.width - divider_rect().x - 1),
                      rect_.height};
    Rect const bounds{0, 0, win->rect().width, win->rect().height};
    win->open_popup(std::move(frame), popup_rect(anchor, {width, height}, bounds));
}

void Cycle::on_mouse_enter(const MouseEvent&) {
    if (enabled_) {
        hovered_ = true;
        damage();
    }
}

void Cycle::on_mouse_leave(const MouseEvent&) {
    if (hovered_) {
        hovered_ = false;
        damage();
    }
}

void Cycle::on_key_down(const KeyEvent& event) {
    if (!enabled_ || !event.pressed) return;
    if (event.code == KeyCode::LEFT) {
        step(-1);
    } else if (event.code == KeyCode::RIGHT) {
        step(1);
    } else if (event.code == KeyCode::HOME) {
        go_to(0);
    } else if (event.code == KeyCode::END) {
        go_to(static_cast<int>(entries_.size()) - 1);
    }
}

}  // namespace aegir::trinket
