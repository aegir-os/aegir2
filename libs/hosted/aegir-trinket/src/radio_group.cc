/*
 * Trinket RadioGroup implementation.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/radio_group.h>

#include <aegir/trinket/canvas.h>
#include <aegir/trinket/diagnostics.h>
#include <aegir/trinket/unicode.h>

#include <memory>

namespace aegir::trinket {

RadioGroup::RadioGroup(Orientation orientation, int spacing)
    : Group(orientation, spacing) {}

RadioGroup::~RadioGroup() = default;

void RadioGroup::report_parts(char const *prefix) const {
    for (size_t i = 0; i < members_.size(); ++i) {
        if (members_[i] != nullptr) {
            report_rect_indexed(prefix, "radio", static_cast<int>(i) + 1,
                                members_[i]->screen_rect());
        }
    }
}

Button* RadioGroup::add(std::u32string_view text) {
    auto button = std::make_unique<Button>(text, Button::Type::RADIO);
    Button* const raw = button.get();
    members_.push_back(raw);
    add_child(std::move(button));
    /* The member reports the click; the group decides what it means. A member's
     * own on_click is the group's, because a radio in a group has exactly one
     * meaning. */
    raw->on_click = [this, raw](bool) { select(raw); };
    if (active_ < 0) {
        set_active(0);
    }
    return raw;
}

Button* RadioGroup::add(std::string_view text) {
    return add(utf8_to_utf32(text));
}

void RadioGroup::select(Button* button) {
    for (int i = 0; i < static_cast<int>(members_.size()); ++i) {
        if (members_[i] == button) {
            set_active(i);
            return;
        }
    }
}

void RadioGroup::set_active(int index) {
    if (index < 0 || index >= static_cast<int>(members_.size())) {
        return;
    }
    bool const changed = index != active_;
    active_ = index;
    for (int i = 0; i < static_cast<int>(members_.size()); ++i) {
        members_[i]->set_checked(i == index);
    }
    if (changed && on_changed) {
        on_changed(index);
    }
}

void RadioGroup::on_key_down(const KeyEvent& event) {
    if (!event.pressed || members_.empty()) {
        return;
    }
    bool const forward = event.code == KeyCode::DOWN || event.code == KeyCode::RIGHT;
    bool const backward = event.code == KeyCode::UP || event.code == KeyCode::LEFT;
    if (!forward && !backward) {
        return;
    }
    /* The arrows wrap, so the end of the group is a step and not a wall. */
    int const count = static_cast<int>(members_.size());
    int next = active_ + (forward ? 1 : -1);
    if (next < 0) {
        next = count - 1;
    } else if (next >= count) {
        next = 0;
    }
    set_active(next);
}

}  // namespace aegir::trinket
