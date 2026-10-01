/*
 * Trinket PopupButton implementation (see the header for the shape).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/popup_button.h>

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/theme.h>

namespace aegir::trinket {

PopupButton::PopupButton(Role role) : role_(role) {}

PopupButton::~PopupButton() = default;

void PopupButton::set_role(Role role) {
    if (role == role_) return;
    role_ = role;
    damage();
}

Size PopupButton::preferred_size() const {
    /* The button is the art's own size (specs/trinket/popup_button.md), so a
     * recipe blits the sprite into it the way the toggles' indicators do. */
    Theme& theme = Application::instance()->theme();
    return {theme.metric(MetricRole::POPUP_BUTTON_WIDTH),
            theme.metric(MetricRole::POPUP_BUTTON_HEIGHT)};
}

Size PopupButton::minimum_size() const { return preferred_size(); }

Size PopupButton::maximum_size() const { return preferred_size(); }

void PopupButton::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);
    static_cast<void>(hovered_);  // the art has no hover frame
    Theme& theme = Application::instance()->theme();
    /* The selected frame is the pressed state (specs/trinket/popup_button.md). */
    theme.draw_popup(canvas, rect_, role_, pressed_ && enabled_);
}

void PopupButton::on_mouse_down(const MouseEvent& event) {
    if (!enabled_) return;
    if (event.button == MouseButton::LEFT) {
        pressed_ = true;
        damage();
    }
}

void PopupButton::on_mouse_up(const MouseEvent& event) {
    if (!enabled_) return;
    if (pressed_ && event.button == MouseButton::LEFT) {
        pressed_ = false;
        damage();
        if (on_click) on_click();
    }
}

void PopupButton::on_mouse_enter(const MouseEvent&) {
    if (enabled_) {
        hovered_ = true;
        damage();
    }
}

void PopupButton::on_mouse_leave(const MouseEvent&) {
    if (hovered_) {
        hovered_ = false;
        damage();
    }
}

void PopupButton::on_key_down(const KeyEvent& event) {
    if (!enabled_) return;
    if (event.code == KeyCode::SPACE || event.code == KeyCode::ENTER) {
        pressed_ = true;
        damage();
    }
}

void PopupButton::on_key_up(const KeyEvent& event) {
    if (!enabled_) return;
    if (pressed_ && (event.code == KeyCode::SPACE || event.code == KeyCode::ENTER)) {
        pressed_ = false;
        damage();
        if (on_click) on_click();
    }
}

}  // namespace aegir::trinket
