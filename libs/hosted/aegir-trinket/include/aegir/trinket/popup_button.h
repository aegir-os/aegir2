/*
 * Trinket PopupButton - a MUI popup image as a button
 * (specs/trinket/popup_button.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_POPUP_BUTTON_H
#define AEGIR_TRINKET_POPUP_BUTTON_H

#include <aegir/trinket/widget.h>
#include <functional>

namespace aegir::trinket {

class Canvas;

/* A small button whose face is a MUI popup image -- `PopUp` (a magnifier),
 * `PopFile` (a document) or `PopDrawer` (a drawer). The XEN art carries its own
 * 3-D face and outline, so the image is the whole button, not an icon on one;
 * the widget reports a click and opens nothing itself, because the object it
 * pops -- a list, a requester -- is the client's (specs/trinket/popup_button.md). */
class PopupButton : public Widget {
public:
    enum class Role { POPUP, FILE, DRAWER };

    explicit PopupButton(Role role = Role::POPUP);
    ~PopupButton() override;

    Role role() const { return role_; }
    void set_role(Role role);

    /* The button was clicked: the pointer released on it, or Space/Enter. */
    std::function<void()> on_click;

    bool focusable() const override { return true; }
    Size preferred_size() const override;
    Size minimum_size() const override;
    Size maximum_size() const override;

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_mouse_down(const MouseEvent& event) override;
    void on_mouse_up(const MouseEvent& event) override;
    void on_mouse_enter(const MouseEvent& event) override;
    void on_mouse_leave(const MouseEvent& event) override;
    void on_key_down(const KeyEvent& event) override;
    void on_key_up(const KeyEvent& event) override;

private:
    Role role_ = Role::POPUP;
    bool pressed_ = false;
    bool hovered_ = false;
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_POPUP_BUTTON_H
