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
class Window;

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

/* The modifier bits `KeyEvent::modifiers` and a menu item's `shortcut_mods`
 * share. A menu accelerator draws these as keycaps (specs/trinket/overview.md). */
constexpr uint32_t kModShift = 1u << 0;
constexpr uint32_t kModControl = 1u << 1;
constexpr uint32_t kModAlt = 1u << 2;
constexpr uint32_t kModSuper = 1u << 3; /* the Windows/Super key */

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
    /* A floor under the size a layout may give this widget: a form aligns a
     * column of labels by handing each the widest one's width, and a label's
     * own preferred is only its text (specs/trinket/layout.md). */
    void set_min_size(Size s) { min_size_ = s; damage(); }
    Size min_size() const { return min_size_; }

    // Visibility and enabled state
    bool visible() const { return visible_; }
    void set_visible(bool v) { if (visible_ != v) { visible_ = v; damage(); } }
    bool enabled() const { return enabled_; }
    void set_enabled(bool e) { enabled_ = e; }

    // Parent/children
    Container* parent() const { return parent_; }
    Container* parent_container() const { return parent_; }
    virtual bool is_container() const { return false; }

    // The window this widget's tree belongs to, found through its parents: the
    // content root is the one that carries it. A widget that opens a popup --
    // the cycle's menu -- needs it (specs/trinket/popup.md).
    Window* window() const;

    // Whether a pointer-down or Tab may focus this widget. A text field and a
    // button are focusable; a label is not.
    virtual bool focusable() const { return false; }

    // The size this widget wants, which layouts arrange it at. The base has no
    // content, so it asks for what it already is; a label or a button overrides
    // it with the size of what it draws.
    virtual Size preferred_size() const;

    // The sizing contract (specs/trinket/layout.md): a layout sizes a child
    // between these. The base is fixed -- the minimum and the maximum are the
    // preferred size -- so a widget reports a range only when it can take one.
    // All three are content-derived, never read back from the rectangle the
    // widget currently holds, so a layout that runs twice computes the same
    // rectangles and the second pass raises no damage.
    virtual Size minimum_size() const;
    virtual Size maximum_size() const;

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

    // Damage / repaint. The no-argument form damages the whole widget; the Rect
    // form damages a region. Two overloads, not one default argument: a default
    // argument makes `damage()` ambiguous against the no-argument form.
    void damage(const Rect& r);
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
    friend class Window;

    Rect rect_;
    Size min_size_{};
    bool visible_ = true;
    bool enabled_ = true;
    bool focused_ = false;
    Container* parent_ = nullptr;
    // The window this widget's tree belongs to, set on the content root by
    // Window::set_content. A damage at the root is the window's to repaint.
    Window* window_ = nullptr;
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

    // A container's size is its layout's: the three come from the children the
    // layout arranges, so a group nested in a group reports the same kind of
    // numbers a leaf does (specs/trinket/layout.md).
    Size preferred_size() const override;
    Size minimum_size() const override;
    Size maximum_size() const override;

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