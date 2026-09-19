/*
 * Trinket Widget base class.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_WIDGET_H
#define AEGIR_TRINKET_WIDGET_H

#include <aegir/trinket/point.h>
#include <aegir/trinket/color.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aegir::trinket {

class Canvas;
class Container;
class Application;

enum class MouseButton { LEFT = 1, MIDDLE = 2, RIGHT = 4 };
enum class KeyCode {
    UNKNOWN = 0,
    BACKSPACE = 8, TAB = 9, ENTER = 13, ESCAPE = 27,
    SPACE = 32,
    LEFT = 0x100, RIGHT, UP, DOWN,
    HOME, END, PAGE_UP, PAGE_DOWN,
    INSERT, DELETE_KEY,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    SHIFT_L, SHIFT_R, CTRL_L, CTRL_R, ALT_L, ALT_R, META_L, META_R
};

struct KeyEvent {
    KeyCode code = KeyCode::UNKNOWN;
    uint32_t modifiers = 0;  // SHIFT=1, CTRL=2, ALT=4, META=8
    char32_t text = 0;       // Unicode codepoint, 0 if not printable
    bool pressed = true;
};

struct MouseEvent {
    Point pos;
    Point global_pos;
    MouseButton button = MouseButton::LEFT;
    uint32_t modifiers = 0;
    int click_count = 1;  // 1=single, 2=double, 3=triple
};

struct PaintEvent {
    Rect clip_rect;
};

class Widget {
public:
    Widget();
    virtual ~Widget();

    // Non-copyable, movable
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;
    Widget(Widget&&) noexcept = default;
    Widget& operator=(Widget&&) noexcept = default;

    // Geometry (logical pixels)
    Rect rect() const { return rect_; }
    void set_rect(Rect r);
    void set_pos(Point p) { rect_.x = p.x; rect_.y = p.y; }
    void set_size(Size s) { rect_.width = s.width; rect_.height = s.height; }

    // Visibility and enabled state
    bool visible() const { return visible_; }
    void set_visible(bool v) { if (visible_ != v) { visible_ = v; damage(); } }
    bool enabled() const { return enabled_; }
    void set_enabled(bool e) { enabled_ = e; }

    // Parent/children
    Container* parent() const { return parent_; }
    Container* parent_container() const { return parent_; }
    virtual bool is_container() const { return false; }

    // Focus
    bool focused() const { return focused_; }
    void set_focused(bool f);

    // Event handlers (override in subclasses)
    virtual void on_paint(Canvas& canvas, const PaintEvent& event);
    virtual void on_mouse_down(const MouseEvent& event);
    virtual void on_mouse_up(const MouseEvent& event);
    virtual void on_mouse_move(const MouseEvent& event);
    virtual void on_mouse_enter(const MouseEvent& event);
    virtual void on_mouse_leave(const MouseEvent& event);
    virtual void on_key_down(const KeyEvent& event);
    virtual void on_key_up(const KeyEvent& event);
    virtual void on_focus_gained();
    virtual void on_focus_lost();
    virtual void on_layout();  // Called after parent layout

    // Damage / repaint
    void damage(const Rect& r = {});
    void damage() { damage(rect_); }

    // Style (can be overridden by theme)
    virtual Color background_color() const { return Color::TRANSPARENT; }
    virtual Color foreground_color() const { return Color::BLACK; }

    // Tooltip
    std::u32string tooltip() const { return tooltip_; }
    void set_tooltip(std::u32string t) { tooltip_ = std::move(t); }

    // Object name (for debugging, stylesheet matching)
    std::string object_name() const { return object_name_; }
    void set_object_name(std::string n) { object_name_ = std::move(n); }

    // Accessibility
    std::u32string accessible_name() const { return accessible_name_; }
    void set_accessible_name(std::u32string n) { accessible_name_ = std::move(n); }
    std::u32string accessible_description() const { return accessible_description_; }
    void set_accessible_description(std::u32string d) { accessible_description_ = std::move(d); }

protected:
    friend class Container;
    friend class Application;

    Rect rect_;
    bool visible_ = true;
    bool enabled_ = true;
    bool focused_ = false;
    Container* parent_ = nullptr;
    std::u32string tooltip_;
    std::string object_name_;
    std::u32string accessible_name_;
    std::u32string accessible_description_;

    // Called by Application to dispatch events
    void dispatch_paint(Canvas& canvas, const PaintEvent& event);
    void dispatch_mouse_down(const MouseEvent& event);
    void dispatch_mouse_up(const MouseEvent& event);
    void dispatch_mouse_move(const MouseEvent& event);
    void dispatch_mouse_enter(const MouseEvent& event);
    void dispatch_mouse_leave(const MouseEvent& event);
    void dispatch_key_down(const KeyEvent& event);
    void dispatch_key_up(const KeyEvent& event);
    void dispatch_focus_gained();
    void dispatch_focus_lost();
    void dispatch_layout();
};

class Container : public Widget {
public:
    Container();
    ~Container() override;

    bool is_container() const override { return true; }

    void add_child(std::unique_ptr<Widget> child);
    void remove_child(Widget* child);
    void clear_children();
    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }
    std::vector<Widget*> children_ptrs();

    void set_layout(std::unique_ptr<class Layout> layout);
    class Layout* layout() const { return layout_.get(); }

    // Find child at point (in container coords)
    Widget* child_at(Point p) const;

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_layout() override;

    std::vector<std::unique_ptr<Widget>> children_;
    std::unique_ptr<class Layout> layout_;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_WIDGET_H