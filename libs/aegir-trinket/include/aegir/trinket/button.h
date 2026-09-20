/*
 * Trinket Button widget.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_BUTTON_H
#define AEGIR_TRINKET_BUTTON_H

#include <aegir/trinket/widget.h>
#include <aegir/trinket/unicode.h>
#include <functional>
#include <string>

namespace aegir::trinket {

class Button : public Widget {
public:
    enum class Type { PUSH, TOGGLE, CHECK, RADIO };

    Button(std::u32string_view text = U"", Type type = Type::PUSH);
    Button(std::string_view text, Type type = Type::PUSH);
    ~Button() override;

    void set_text(std::u32string_view text);
    void set_text(std::string_view text);
    const std::u32string& text() const { return text_; }

    void set_type(Type t) { type_ = t; damage(); }
    Type type() const { return type_; }

    bool checked() const { return checked_; }
    void set_checked(bool c) { if (checked_ != c) { checked_ = c; damage(); } }

    // Callback when button is clicked (for push buttons) or toggled
    std::function<void(bool checked)> on_click;
    std::function<void()> on_pressed;
    std::function<void()> on_released;

    void set_icon(std::u32string_view icon) { icon_ = std::u32string(icon); damage(); }
    void set_icon(std::string_view icon);

    // Auto-repeat for held buttons
    void set_auto_repeat(bool enable, int interval_ms = 100);

    bool focusable() const override { return true; }

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_mouse_down(const MouseEvent& event) override;
    void on_mouse_up(const MouseEvent& event) override;
    void on_mouse_enter(const MouseEvent& event) override;
    void on_mouse_leave(const MouseEvent& event) override;
    void on_key_down(const KeyEvent& event) override;
    void on_key_up(const KeyEvent& event) override;
    void on_focus_gained() override;
    void on_focus_lost() override;
    Size preferred_size() const override;

private:
    std::u32string text_;
    std::u32string icon_;
    Type type_ = Type::PUSH;
    bool checked_ = false;
    bool pressed_ = false;
    bool hovered_ = false;
    bool auto_repeat_ = false;
    int repeat_interval_ = 100;
    uint64_t repeat_timer_ = 0;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_BUTTON_H