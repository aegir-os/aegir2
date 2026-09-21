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

/* A titlebar gadget, drawn on the bar (its plate is the bar's own fill) with
 * the Workbench glyphs (specs/amiga-fidelity.md): a near-black outline, and
 * white and grey interiors. When the window is not active every fill becomes
 * the bar's colour, so the glyph reads hollow. */
void draw_gadget(Canvas& canvas, Rect const& r, int kind, bool active,
                 Color bar, Color outline, Color white, Color grey) {
    Color const inner = active ? white : bar;
    if (kind == 1) {
        /* Close: a small square, white fill, centred. */
        Rect const box{r.x + 4, r.y + 4, 8, 8};
        canvas.fill_rect(box, inner);
        canvas.draw_rect(box, outline);
    } else if (kind == 2) {
        /* Zoom: a box in a box -- outer the bar's fill, inner upper-left. */
        Rect const outer{r.x + 2, r.y + 2, 12, 12};
        canvas.fill_rect(outer, bar);
        canvas.draw_rect(outer, outline);
        Rect const box{r.x + 4, r.y + 4, 6, 6};
        canvas.fill_rect(box, inner);
        canvas.draw_rect(box, outline);
    } else {
        /* Depth: two cascaded squares, upper grey, lower white. */
        Rect const upper{r.x + 2, r.y + 2, 10, 10};
        canvas.fill_rect(upper, active ? grey : bar);
        canvas.draw_rect(upper, outline);
        Rect const lower{r.x + 5, r.y + 5, 10, 10};
        canvas.fill_rect(lower, inner);
        canvas.draw_rect(lower, outline);
    }
}

/* The resize gadget: a white right triangle, near-black outline, right angle
 * at the bottom-right, with a white line down its left separating it from the
 * bar (specs/amiga-fidelity.md). */
void draw_resize_gadget(Canvas& canvas, Rect const& r, Color white,
                        Color outline) {
    int const w = r.width;
    int const h = r.height;
    canvas.draw_vline(r.y, r.y + h - 1, r.x, white);
    for (int row = 0; row < h; ++row) {
        int const left =
            r.x + (w - 1) * (h - 1 - row) / (h > 1 ? h - 1 : 1);
        canvas.draw_hline(left, r.x + w - 1, r.y + row, white);
    }
    canvas.draw_vline(r.y, r.y + h - 1, r.x + w - 1, outline);
    canvas.draw_hline(r.x, r.x + w - 1, r.y + h - 1, outline);
    canvas.draw_line({r.x + w - 1, r.y}, {r.x, r.y + h - 1}, outline);
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

void Window::request_focus() {
    want_focus_ = true;
    if (console_window_id_ != 0) {
        (void)aegir::console::focus(app_.gui_port(), console_window_id_);
    }
}

void Window::damage(const Rect& r) {
    /* The rectangle arrives in the content's coordinates; the frame is the
     * content plus the titlebar above it. An empty rectangle is the whole
     * window; otherwise the event's damages are unioned into one repaint. */
    Rect frame_rect = r;
    if (frame_rect.empty()) {
        Rect const frame = frame_for(rect_);
        frame_rect = {0, 0, frame.width, frame.height};
    } else {
        frame_rect.y += titlebar_height();
    }
    damage_rect_ = damage_rect_.empty() ? frame_rect : damage_rect_.united(frame_rect);
    /* A geometry change collects its damage and repaints once, after the
     * console's window has the new size: a repaint here would read the
     * backing at the new stride before it was drawn at it. */
    if (!geometry_change_) {
        repaint();
    }
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
            /* The console sends the pointer's *screen* position during a grab
             * (specs/window-manager.md): the frame's origin is that minus
             * where the pointer sat in the frame when the drag began. Using
             * the window's own origin here was the "throw", because a queued
             * event's coordinates are relative to where the window was when
             * the console delivered it, not where it is now. */
            int const nx = pos.x - drag_offset_x_;
            int const ny = pos.y - drag_offset_y_ + bar;
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
            /* The pointer's screen delta from where the resize began, added
             * to the size it began at: absolute, so a queued motion reads
             * against a fixed origin. */
            int const wanted_w = resize_start_width_ + (pos.x - resize_origin_x_);
            int const wanted_h = resize_start_height_ + (pos.y - resize_origin_y_);
            int const w = wanted_w < min_w ? min_w : wanted_w;
            int const h = wanted_h < min_h ? min_h : wanted_h;
            if (w != rect_.width || h != rect_.height) {
                Rect const resized{rect_.x, rect_.y, w, h};
                Rect const frame = frame_for(resized);
                DisplayInfo const& display = app_.display_info();
                /* The screen bounds it; off the screen is refused, and the
                 * window stops growing. The check is here, before anything
                 * changes, so a refusal leaves nothing half-done. */
                bool const fits =
                    display.width_px > 0 && display.height_px > 0 &&
                    frame.x >= 0 && frame.y >= 0 &&
                    frame.x + frame.width <= static_cast<int>(display.width_px) &&
                    frame.y + frame.height <= static_cast<int>(display.height_px);
                if (fits) {
                    /* The new frame is painted *before* the console is told
                     * its size, so the console's resize composites a backing
                     * already drawn at the new stride; the content's own
                     * damage is collected, not repainted (geometry_change_),
                     * and the console's resize is the one composite. */
                    geometry_change_ = true;
                    rect_.width = w;
                    rect_.height = h;
                    if (content_) content_->set_rect({0, 0, w, h});
                    geometry_change_ = false;
                    damage_rect_ = {};
                    paint();
                    if (aegir::console::resize(app_.gui_port(), console_window_id_,
                                               static_cast<uint64_t>(frame.width),
                                               static_cast<uint64_t>(frame.height))) {
                        if (on_moved_resized) on_moved_resized(rect_);
                    }
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

    /* A titlebar gadget: close, zoom or depth. The titlebar elsewhere drags and
     * raises. */
    int const gadget = gadget_at(pos);
    if (gadget != 0) {
        if (gadget == 1) {
            close();
        } else if (gadget == 2) {
            zoom();
        } else {
            (void)aegir::console::lower(app_.gui_port(), console_window_id_);
        }
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

    /* The resize gadget in the bottom bar begins a resize. */
    if (resize_gadget_rect().contains(pos) && bottombar_height() > 0) {
        resizing_ = true;
        /* The pointer's screen position: the down is window-local, and the
         * frame's origin is (rect_.x, rect_.y - bar). */
        resize_origin_x_ = rect_.x + pos.x;
        resize_origin_y_ = rect_.y - bar + pos.y;
        resize_start_width_ = rect_.width;
        resize_start_height_ = rect_.height;
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

/* A resizable decorated window carries a bottom bar of the titlebar's height
 * and fill, with the resize gadget in its lower right (specs/amiga-fidelity.md). */
int Window::bottombar_height() const {
    return decorated_ && resizable_
               ? app_.theme().metric(MetricRole::TITLEBAR_HEIGHT)
               : 0;
}

Rect Window::frame_for(const Rect& content) const {
    int const bar = titlebar_height();
    int const bottom = bottombar_height();
    return {content.x, content.y - bar, content.width, content.height + bar + bottom};
}

void Window::set_gadgets(bool close, bool zoom, bool depth) {
    gadget_close_ = close;
    gadget_zoom_ = zoom;
    gadget_depth_ = depth;
    repaint();
}

/* The right-packed gadget slot, index 0 the rightmost (Depth), then Zoom
 * (specs/amiga-fidelity.md). */
Rect Window::gadget_rect(int index_from_right) const {
    Theme& theme = app_.theme();
    int const size = theme.metric(MetricRole::TITLEBAR_BUTTON_SIZE);
    int const pad = theme.metric(MetricRole::TITLEBAR_PADDING_H);
    int const gap = theme.metric(MetricRole::SPACING_SMALL);
    int const bar = titlebar_height();
    int const right = rect_.width - pad - (index_from_right + 1) * size -
                      index_from_right * gap;
    return {right, (bar - size) / 2, size, size};
}

/* Close sits at the titlebar's far left; Zoom and Depth are packed at its
 * right, Depth rightmost (specs/amiga-fidelity.md). */
Rect Window::close_gadget_rect() const {
    Theme& theme = app_.theme();
    int const size = theme.metric(MetricRole::TITLEBAR_BUTTON_SIZE);
    int const pad = theme.metric(MetricRole::TITLEBAR_PADDING_H);
    int const bar = titlebar_height();
    return {pad, (bar - size) / 2, size, size};
}

/* The resize gadget: the bottom bar's lower-right square. */
Rect Window::resize_gadget_rect() const {
    int const size = bottombar_height();
    Rect const frame = frame_for(rect_);
    return {frame.width - size, frame.height - size, size, size};
}

int Window::gadget_at(Point p) const {
    if (!decorated_) return 0;
    if (gadget_close_ && close_gadget_rect().contains(p)) return 1;
    int index = 0;
    if (gadget_depth_) {
        if (gadget_rect(index).contains(p)) return 3;
        ++index;
    }
    if (gadget_zoom_) {
        if (gadget_rect(index).contains(p)) return 2;
        ++index;
    }
    return 0;
}

/* Zoom toggles between where the window was and the whole screen, its
 * titlebar at the top. The screen's size is the bound a resize may reach
 * (specs/window-manager.md).
 *
 * The console composites a window's backing with the window's width as its
 * stride, so a resize must be told only after the backing has been drawn at
 * the new size. Growing moves first (with the old size, whose backing is
 * still drawn at the old stride), paints, then resizes; shrinking paints the
 * new size, resizes at the old origin, then moves to the saved one. */
void Window::zoom() {
    if (console_window_id_ == 0) return;
    DisplayInfo const& display = app_.display_info();
    int const bar = titlebar_height();
    int const bottom = bottombar_height();
    if (!zoomed_) {
        if (display.width_px == 0 || display.height_px == 0) return;
        /* The whole screen is the frame: the content is what is left after
         * both bars, or the frame would not fit and the paint would run past
         * the backing. */
        Rect const target{0, bar, static_cast<int>(display.width_px),
                          static_cast<int>(display.height_px) - bar - bottom};
        if (target.width <= 0 || target.height <= 0) return;
        Rect const target_frame = frame_for(target);
        if (!aegir::console::move(app_.gui_port(), console_window_id_,
                                  static_cast<uint64_t>(target_frame.x),
                                  static_cast<uint64_t>(target_frame.y))) {
            return;
        }
        saved_rect_ = rect_;
        geometry_change_ = true;
        rect_ = target;
        if (content_) content_->set_rect({0, 0, target.width, target.height});
        geometry_change_ = false;
        damage_rect_ = {};
        paint();
        if (!aegir::console::resize(app_.gui_port(), console_window_id_,
                                    static_cast<uint64_t>(target_frame.width),
                                    static_cast<uint64_t>(target_frame.height))) {
            return;
        }
        zoomed_ = true;
    } else {
        Rect const target = saved_rect_;
        Rect const target_frame = frame_for(target);
        geometry_change_ = true;
        rect_ = target;
        if (content_) content_->set_rect({0, 0, target.width, target.height});
        geometry_change_ = false;
        damage_rect_ = {};
        paint();
        if (!aegir::console::resize(app_.gui_port(), console_window_id_,
                                    static_cast<uint64_t>(target_frame.width),
                                    static_cast<uint64_t>(target_frame.height))) {
            return;
        }
        if (!aegir::console::move(app_.gui_port(), console_window_id_,
                                  static_cast<uint64_t>(target_frame.x),
                                  static_cast<uint64_t>(target_frame.y))) {
            return;
        }
        zoomed_ = false;
    }
    if (on_moved_resized) on_moved_resized(rect_);
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
        /* The largest frame the console accepts is the screen: the content is
         * what is left after both bars (specs/window-manager.md). */
        content_height = static_cast<int>(display.height_px) - titlebar_height() -
                         bottombar_height();
        if (content_height < 1) {
            content_height = static_cast<int>(display.height_px);
        }
    }
    Rect const frame = frame_for({0, 0, content_width, content_height});
    return static_cast<uint64_t>(frame.width) *
           static_cast<uint64_t>(frame.height) * 4ull;
}

Rect Window::paint() {
    if (!visible_ || console_window_id_ == 0 || app_.slice() == nullptr) return {};

    Rect const frame = frame_for(rect_);
    int const bar = titlebar_height();
    int const bottom = bottombar_height();
    /* Only the region an event damaged is painted and handed the console:
     * repainting the whole frame on every keystroke is what made typing
     * crawl (specs/trinket.md deferred this; specs/window-manager.md's
     * partial damage). An empty damage is the whole frame, window-local. */
    Rect const damage =
        damage_rect_.empty() ? Rect{0, 0, frame.width, frame.height} : damage_rect_;
    damage_rect_ = {};
    uint32_t* const pixels =
        reinterpret_cast<uint32_t*>(app_.slice() + backing_offset_);

    /* The client-side frame (specs/amiga-fidelity.md): a beveled title bar, the
     * content, and, for a resizable window, a beveled bottom bar with the
     * resize gadget. The content is painted through a canvas offset by the
     * titlebar, so its coordinates are its own. */
    Theme& theme = app_.theme();
    Canvas frame_canvas(pixels, frame.width, frame.height, frame.width);
    frame_canvas.set_clip_rect(damage);
    frame_canvas.fill_rect({0, 0, frame.width, frame.height},
                           theme.color(ColorRole::WINDOW_BG));

    if (bar > 0) {
        frame_canvas.fill_rect({0, 0, frame.width, bar},
                               theme.color(ColorRole::TITLEBAR_BG));
        if (bar > 2) {
            frame_canvas.draw_hline(0, frame.width - 1, 1,
                                    theme.color(ColorRole::TITLEBAR_HIGHLIGHT));
        }
        frame_canvas.draw_hline(0, frame.width - 1, bar - 1,
                                theme.color(ColorRole::TITLEBAR_SHADOW));

        Font* const font = app_.default_font();
        if (font != nullptr && !title_.empty()) {
            int const pad = theme.metric(MetricRole::TITLEBAR_PADDING_H);
            int const size = theme.metric(MetricRole::TITLEBAR_BUTTON_SIZE);
            int const gap = theme.metric(MetricRole::SPACING_SMALL);
            int const x = gadget_close_ ? pad + size + gap : pad;
            std::string const title = utf32_to_utf8(title_);
            frame_canvas.draw_text({x, (bar - font->height()) / 2}, title, font,
                                   theme.color(ColorRole::TITLEBAR_TEXT));
        }

        Color const outline = theme.color(ColorRole::GADGET_OUTLINE);
        Color const white = theme.color(ColorRole::GADGET_WHITE);
        Color const grey = theme.color(ColorRole::GADGET_GREY);
        Color const bar_fill = theme.color(ColorRole::TITLEBAR_BG);
        if (gadget_close_) {
            draw_gadget(frame_canvas, close_gadget_rect(), 1, active_, bar_fill,
                        outline, white, grey);
        }
        int index = 0;
        if (gadget_depth_) {
            draw_gadget(frame_canvas, gadget_rect(index), 3, active_, bar_fill,
                        outline, white, grey);
            ++index;
        }
        if (gadget_zoom_) {
            draw_gadget(frame_canvas, gadget_rect(index), 2, active_, bar_fill,
                        outline, white, grey);
            ++index;
        }
    }

    if (content_) {
        content_->dispatch_layout();
        Canvas content_canvas(pixels + static_cast<size_t>(bar) * frame.width,
                              rect_.width, rect_.height, frame.width);
        /* The content's own space: the damage shifts down by the titlebar. */
        content_->dispatch_paint(
            content_canvas,
            PaintEvent{{damage.x, damage.y - bar, damage.width, damage.height}});
    }

    if (bottom > 0) {
        int const top = frame.height - bottom;
        frame_canvas.fill_rect({0, top, frame.width, bottom},
                               theme.color(ColorRole::TITLEBAR_BG));
        if (bottom > 2) {
            frame_canvas.draw_hline(0, frame.width - 1, top + 1,
                                    theme.color(ColorRole::BOTTOMBAR_HIGHLIGHT));
        }
        frame_canvas.draw_hline(0, frame.width - 1, frame.height - 2,
                                theme.color(ColorRole::BOTTOMBAR_SHADOW));
        draw_resize_gadget(frame_canvas, resize_gadget_rect(),
                           theme.color(ColorRole::GADGET_WHITE),
                           theme.color(ColorRole::GADGET_OUTLINE));
    }

    if (decorated_) {
        theme.draw_window_frame(frame_canvas, {0, 0, frame.width, frame.height},
                                active_);
    }
    return damage;
}

void Window::repaint() {
    Rect const damage = paint();
    if (damage.empty()) return;
    (void)aegir::console::damage(app_.gui_port(), console_window_id_,
                                 static_cast<uint64_t>(damage.x),
                                 static_cast<uint64_t>(damage.y),
                                 static_cast<uint64_t>(damage.width),
                                 static_cast<uint64_t>(damage.height));
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
    if (want_focus_) {
        (void)aegir::console::focus(app_.gui_port(), console_window_id_);
    }
    /* The first paint is the whole frame: damages recorded before the window
     * existed (a set_content before show) are not a region to clip to. */
    damage_rect_ = {};
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
