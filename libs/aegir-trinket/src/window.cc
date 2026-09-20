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

/* The resize grip's size, frame-local, in the bottom-right corner of the
 * content (specs/window-manager.md). */
constexpr int kResizeGrip = 16;

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
    repaint();
}

void Window::set_title(std::string_view title) {
    title_ = utf8_to_utf32(title);
    repaint();
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
         * climbs to it is a repaint request here. Its rectangle is the
         * content's own coordinate space; the window translates the canvas
         * when it paints, so widgets do not know about the titlebar. */
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
    /* The titlebar's active colour follows the console's focus. */
    if (!active_) {
        active_ = true;
        repaint();
    }
    if (on_focus_changed) on_focus_changed(true);
}

void Window::on_focus_lost() {
    if (active_) {
        active_ = false;
        repaint();
    }
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

    /* Motion carries no button. During a gesture it moves or resizes the
     * frame: the console delivers motion to the grab holder frame-local. */
    uint16_t const button =
        static_cast<uint16_t>(code & ~aegir::console::kButtonRelease);
    int const bar = titlebar_height();
    if (button == 0) {
        if (dragging_) {
            int const nx = rect_.x + pos.x - drag_offset_x_;
            int const ny = rect_.y + pos.y - drag_offset_y_;
            if (nx != rect_.x || ny != rect_.y) {
                Rect const moved{nx, ny, rect_.width, rect_.height};
                Rect const frame = frame_for(moved);
                /* A move off the screen is refused; the window stops at the
                 * edge and the drag goes on. */
                if (aegir::console::move(app_.gui_port(), console_window_id_,
                                         static_cast<uint64_t>(frame.x),
                                         static_cast<uint64_t>(frame.y))) {
                    rect_ = moved;
                    if (on_moved_resized) on_moved_resized(moved);
                }
            }
        } else if (resizing_) {
            int const min_w = min_size_.width > 0 ? min_size_.width : 1;
            int const min_h = min_size_.height > 0 ? min_size_.height : 1;
            int const wanted_w = rect_.width + (pos.x - resize_offset_x_);
            int const wanted_h = rect_.height + (pos.y - resize_offset_y_);
            int const w = wanted_w < min_w ? min_w : wanted_w;
            int const h = wanted_h < min_h ? min_h : wanted_h;
            if (w != rect_.width || h != rect_.height) {
                Rect const resized{rect_.x, rect_.y, w, h};
                Rect const frame = frame_for(resized);
                /* Off the screen is refused; the window stops growing. */
                if (aegir::console::resize(app_.gui_port(), console_window_id_,
                                           static_cast<uint64_t>(frame.width),
                                           static_cast<uint64_t>(frame.height))) {
                    rect_.width = w;
                    rect_.height = h;
                    resize_offset_x_ = pos.x;
                    resize_offset_y_ = pos.y;
                    if (content_) content_->set_rect({0, 0, w, h});
                    if (on_moved_resized) on_moved_resized(rect_);
                    repaint();
                }
            }
        }
        return;
    }

    bool const up = (code & aegir::console::kButtonRelease) != 0;
    /* The console's coordinates are frame-local; the content starts below the
     * titlebar, so widget dispatch works in the content's own space. */
    Point const content_pos{pos.x, pos.y - bar};
    MouseEvent mouse;
    mouse.pos = content_pos;
    mouse.global_pos = content_pos;  // content-local is all the toolkit has
    /* The console passes the HID button codes through (aegir/input.h): left
     * is 0x110, not 1. */
    switch (button) {
    case aegir::input::kBtnLeft: mouse.button = MouseButton::LEFT; break;
    case aegir::input::kBtnRight: mouse.button = MouseButton::RIGHT; break;
    case aegir::input::kBtnMiddle: mouse.button = MouseButton::MIDDLE; break;
    default: return;
    }

    if (up) {
        if (dragging_ || resizing_) {
            dragging_ = false;
            resizing_ = false;
            return;
        }
        Widget* const target = hit_test(content_.get(), content_pos);
        if (target != nullptr) target->dispatch_mouse_up(mouse);
        return;
    }

    /* A pointer-down in the titlebar begins a drag and raises: a deliberate
     * act brings the window forward, where a click alone only focuses it. */
    if (decorated_ && pos.y < bar) {
        dragging_ = true;
        drag_offset_x_ = pos.x;
        drag_offset_y_ = pos.y;
        (void)aegir::console::raise(app_.gui_port(), console_window_id_);
        return;
    }

    /* The bottom-right grip begins a resize. */
    if (decorated_ && pos.x >= rect_.width - kResizeGrip &&
        pos.y >= bar + rect_.height - kResizeGrip) {
        resizing_ = true;
        resize_offset_x_ = pos.x;
        resize_offset_y_ = pos.y;
        (void)aegir::console::raise(app_.gui_port(), console_window_id_);
        return;
    }

    Widget* const target = hit_test(content_.get(), content_pos);
    if (target == nullptr) return;
    if (target->focusable()) set_focus(target);
    target->dispatch_mouse_down(mouse);
}

int Window::titlebar_height() const {
    return decorated_ ? app_.theme().metric(MetricRole::TITLEBAR_HEIGHT) : 0;
}

Rect Window::frame_for(const Rect& content) const {
    int const bar = titlebar_height();
    return {content.x, content.y - bar, content.width, content.height + bar};
}

uint64_t Window::backing_bytes() const {
    /* The screen-bounded maximum: attach carves one slice per badge and a
     * second is refused, so a resizable window reserves the largest frame the
     * console will accept (specs/window-manager.md). */
    int content_width = rect_.width;
    int content_height = rect_.height;
    DisplayInfo const& display = app_.display_info();
    if (display.width_px > 0 && display.height_px > 0) {
        content_width = static_cast<int>(display.width_px);
        content_height = static_cast<int>(display.height_px) - titlebar_height();
        if (content_height < 1) {
            content_height = static_cast<int>(display.height_px);
        }
    }
    Rect const frame = frame_for({0, 0, content_width, content_height});
    return static_cast<uint64_t>(frame.width) *
           static_cast<uint64_t>(frame.height) * 4ull;
}

void Window::repaint() {
    if (!visible_ || console_window_id_ == 0 || app_.slice() == nullptr) return;

    Rect const frame = frame_for(rect_);
    int const bar = titlebar_height();
    uint32_t* const pixels =
        reinterpret_cast<uint32_t*>(app_.slice() + backing_offset_);

    /* The client-side frame: the window's own surface, the titlebar over the
     * content, and the theme's border around the whole rectangle
     * (specs/window-manager.md). The content is painted through a canvas
     * offset by the titlebar, so its coordinates are its own. */
    Theme& theme = app_.theme();
    Canvas frame_canvas(pixels, frame.width, frame.height, frame.width);
    frame_canvas.fill_rect({0, 0, frame.width, frame.height},
                           theme.color(ColorRole::WINDOW_BG));
    if (bar > 0) {
        std::string const title = utf32_to_utf8(title_);
        theme.draw_titlebar(frame_canvas, {0, 0, frame.width, bar}, title.c_str(),
                            active_);
    }
    if (content_) {
        content_->dispatch_layout();
        Canvas content_canvas(pixels + static_cast<size_t>(bar) * frame.width,
                              rect_.width, rect_.height, frame.width);
        content_->dispatch_paint(content_canvas,
                                 PaintEvent{{0, 0, rect_.width, rect_.height}});
    }
    if (bar > 0) {
        theme.draw_window_frame(frame_canvas, {0, 0, frame.width, frame.height},
                                active_);
    }

    (void)aegir::console::damage(app_.gui_port(), console_window_id_, 0, 0,
                                 static_cast<uint64_t>(frame.width),
                                 static_cast<uint64_t>(frame.height));
}

void Window::create_bureau_window() {
    if (console_window_id_ != 0) return;
    /* Before Application::exec attaches the slice there is nothing to draw
     * into; exec creates the windows that were shown early, so a show() before
     * it is not an error. */
    if (!app_.gui_port().valid() || app_.slice() == nullptr) return;

    uint64_t const bytes = backing_bytes();
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

    /* The console's window is the frame; the content sits below the titlebar
     * inside it (specs/window-manager.md). */
    Rect const frame = frame_for(rect_);
    console_window_id_ = aegir::console::create_window(
        app_.gui_port(),
        static_cast<uint64_t>(frame.x), static_cast<uint64_t>(frame.y),
        static_cast<uint64_t>(frame.width), static_cast<uint64_t>(frame.height),
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
    if (console_window_id_ == 0) return;
    Rect const frame = frame_for(rect_);
    (void)aegir::console::move(app_.gui_port(), console_window_id_,
                               static_cast<uint64_t>(frame.x),
                               static_cast<uint64_t>(frame.y));
}

void Window::register_menubar() {
    // If this window has a menubar, register it with Bureau
    // TODO: Implement menubar registration
}

void Window::unregister_menubar() {
    // TODO
}

} // namespace aegir::trinket
