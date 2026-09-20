/*
 * Trinket Window implementation.
 */

#include <aegir/trinket/window.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/theme.h>
#include <aegir/console.h>
#include <aegir/input.h>
#include <aegir/ipc/port.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>
#include <algorithm>
#include <vector>

namespace aegir::trinket {

namespace {

/* The topmost widget under `p`, window-local. Rects are window-absolute (the
 * layouts place children against the container's own rect), so the same point
 * descends the tree unchanged. */
Widget* hit_test(Widget* widget, Point p) {
    if (widget == nullptr || !widget->visible()) return nullptr;
    if (widget->is_container()) {
        Container* const container = static_cast<Container*>(widget);
        Widget* const child = container->child_at(p);
        if (child != nullptr) {
            Widget* const deeper = hit_test(child, p);
            return deeper != nullptr ? deeper : child;
        }
    }
    return widget->rect().contains(p) ? widget : nullptr;
}

/* The focusables in the tree, in tree order: what Tab cycles through. */
void collect_focusables(Widget* widget, std::vector<Widget*>& out) {
    if (widget == nullptr || !widget->visible()) return;
    if (widget->is_container()) {
        Container* const container = static_cast<Container*>(widget);
        for (const auto& child : container->children()) {
            collect_focusables(child.get(), out);
        }
    }
    if (widget->focusable()) out.push_back(widget);
}

}  // namespace

Window::Window(Application& app)
    : app_(app), console_window_id_(0), frame_window_id_(0) {
    app_.register_window(this);
}

Window::~Window() {
    destroy_bureau_window();
    app_.unregister_window(this);
}

void Window::set_title(std::u32string_view title) {
    title_ = std::u32string(title);
    update_bureau_window();
}

void Window::set_title(std::string_view title) {
    title_ = utf8_to_utf32(title);
    update_bureau_window();
}

void Window::set_rect(Rect r) {
    if (rect_ != r) {
        rect_ = r;
        update_bureau_window();
        if (on_moved_resized) on_moved_resized(r);
    }
}

void Window::set_decorated(bool decorated) {
    if (decorated_ != decorated) {
        decorated_ = decorated;
        // Recreate window with new decoration state
        if (visible_) {
            destroy_bureau_window();
            create_bureau_window();
        }
    }
}

void Window::set_content(std::unique_ptr<Widget> content) {
    content_ = std::move(content);
    if (content_) {
        /* The content root is the tree's link to this window: a damage that
         * climbs to it is a repaint request here. */
        content_->window_ = this;
        content_->set_rect({0, 0, rect_.width, rect_.height});
        content_->dispatch_layout();
        repaint();
    }
}

void Window::show() {
    if (!visible_) {
        visible_ = true;
        create_bureau_window();
        if (on_shown) on_shown();
    }
}

void Window::hide() {
    if (visible_) {
        visible_ = false;
        destroy_bureau_window();
        if (on_hidden) on_hidden();
    }
}

void Window::close() {
    if (visible_) {
        if (on_close_requested) on_close_requested();
        hide();
    }
}

void Window::damage(const Rect& r) {
    static_cast<void>(r);  // tier 1 repaints the whole window
    repaint();
}

void Window::on_focus_gained() {
    // Swap menubar content
    // This would be handled by Bureau's menubar server
    if (on_focus_changed) on_focus_changed(true);
}

void Window::on_focus_lost() {
    if (on_focus_changed) on_focus_changed(false);
}

void Window::set_focus(Widget* widget) {
    if (focused_ == widget) return;
    if (focused_ != nullptr) focused_->set_focused(false);
    focused_ = widget;
    if (focused_ != nullptr) focused_->set_focused(true);
}

void Window::focus_next() {
    std::vector<Widget*> focusables;
    collect_focusables(content_.get(), focusables);
    if (focusables.empty()) return;
    auto it = std::find(focusables.begin(), focusables.end(), focused_);
    if (it == focusables.end() || ++it == focusables.end()) {
        set_focus(focusables.front());
    } else {
        set_focus(*it);
    }
}

void Window::dispatch_key(uint64_t event) {
    uint32_t const value = aegir::input::event_value(event);
    char const c = static_cast<char>(value & 0xffff);
    bool const pressed = (value & aegir::console::kKeyPressed) != 0;

    /* The console hands over the translated character; the few keys the
     * toolkit acts on get a KeyCode as well. Tab is the window's, because it
     * moves the focus rather than reaching a widget. */
    KeyEvent key;
    key.pressed = pressed;
    switch (c) {
    case '\b': key.code = KeyCode::BACKSPACE; break;
    case '\t': key.code = KeyCode::TAB; break;
    case '\n': key.code = KeyCode::ENTER; break;
    case 27: key.code = KeyCode::ESCAPE; break;
    case ' ': key.code = KeyCode::SPACE; key.text = U' '; break;
    default:
        if (c >= 32 && c < 127) key.text = static_cast<char32_t>(c);
        break;
    }

    if (key.code == KeyCode::TAB) {
        if (pressed) focus_next();
        return;
    }
    if (focused_ == nullptr) return;
    if (pressed) {
        focused_->dispatch_key_down(key);
    } else {
        focused_->dispatch_key_up(key);
    }
}

void Window::dispatch_pointer(uint64_t event) {
    uint32_t const value = aegir::input::event_value(event);
    uint16_t const code = aegir::input::event_code(event);
    Point const pos{static_cast<int>(value & 0xffff),
                    static_cast<int>((value >> 16) & 0xffff)};

    bool const up = (code & aegir::console::kButtonRelease) != 0;
    uint16_t const button =
        static_cast<uint16_t>(code & ~aegir::console::kButtonRelease);
    MouseEvent mouse;
    mouse.pos = pos;
    mouse.global_pos = pos;  // window-local is all the toolkit has
    /* The console passes the HID button codes through (aegir/input.h): left
     * is 0x110, not 1. Motion (no button) is not dispatched in tier 1. */
    switch (button) {
    case aegir::input::kBtnLeft: mouse.button = MouseButton::LEFT; break;
    case aegir::input::kBtnRight: mouse.button = MouseButton::RIGHT; break;
    case aegir::input::kBtnMiddle: mouse.button = MouseButton::MIDDLE; break;
    default: return;
    }

    Widget* const target = hit_test(content_.get(), pos);
    if (target == nullptr) return;
    if (up) {
        target->dispatch_mouse_up(mouse);
    } else {
        if (target->focusable()) set_focus(target);
        target->dispatch_mouse_down(mouse);
    }
}

void Window::repaint() {
    if (!visible_ || console_window_id_ == 0 || app_.slice() == nullptr) return;

    uint32_t* const pixels =
        reinterpret_cast<uint32_t*>(app_.slice() + backing_offset_);
    canvas_ = Canvas(pixels, rect_.width, rect_.height, rect_.width);

    if (content_) {
        content_->dispatch_layout();
        content_->dispatch_paint(canvas_, PaintEvent{{0, 0, rect_.width, rect_.height}});
    }

    (void)aegir::console::damage(app_.gui_port(), console_window_id_, 0, 0,
                                 static_cast<uint64_t>(rect_.width),
                                 static_cast<uint64_t>(rect_.height));
}

void Window::create_bureau_window() {
    if (console_window_id_ != 0) return;
    /* Before Application::exec attaches the slice there is nothing to draw
     * into; exec creates the windows that were shown early, so a show() before
     * it is not an error. */
    if (!app_.gui_port().valid() || app_.slice() == nullptr) return;

    uint64_t const bytes = static_cast<uint64_t>(rect_.width) *
                           static_cast<uint64_t>(rect_.height) * 4ull;
    if (backing_offset_ == ~0ull) {
        backing_offset_ = app_.claim_backing(bytes);
    }
    if (backing_offset_ == ~0ull) {
        aegir::debug_write("Window: no backing left in the slice\n");
        return;
    }

    uint64_t flags = 0;
    if (!decorated_) {
        // No decorations - just a raw window
        flags = aegir::console::kWindowBackdrop;
    }

    console_window_id_ = aegir::console::create_window(
        app_.gui_port(),
        rect_.x, rect_.y, rect_.width, rect_.height,
        backing_offset_,
        flags);

    if (console_window_id_ == 0) {
        aegir::debug_write("Window: Failed to create console window\n");
        return;
    }

    register_menubar();
    repaint();
}

void Window::destroy_bureau_window() {
    if (console_window_id_) {
        unregister_menubar();
        aegir::console::destroy_window(app_.gui_port(), console_window_id_);
        console_window_id_ = 0;
    }
    if (frame_window_id_) {
        // Destroy frame window if it exists
        frame_window_id_ = 0;
    }
}

void Window::update_bureau_window() {
    if (console_window_id_) {
        // Update window position/size
        // This would require a new console protocol method
    }
}

void Window::register_menubar() {
    // If this window has a menubar, register it with Bureau
    // TODO: Implement menubar registration
}

void Window::unregister_menubar() {
    // TODO
}

} // namespace aegir::trinket
