/*
 * Trinket Window - wrapper around console.gui + bureau.wm.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_WINDOW_H
#define AEGIR_TRINKET_WINDOW_H

#include <aegir/trinket/widget.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/unicode.h>
#include <aegir/console.h>
#include <functional>
#include <memory>

namespace aegir::trinket {

class Window {
public:
    explicit Window(Application& app);
    ~Window();

    void set_title(std::u32string_view title);
    void set_title(std::string_view title);
    const std::u32string& title() const { return title_; }

    void set_rect(Rect r);  // The content's rectangle, in screen coordinates
    Rect rect() const { return rect_; }

    // The bytes the window's backing needs. The backing is the *frame*: a
    // decorated window adds the titlebar above the content, so its backing is
    // (height + titlebar_height) tall. Application sizes its slice from the
    // sum over its visible windows.
    uint64_t backing_bytes() const;

    void set_decorated(bool decorated);  // Default true
    bool decorated() const { return decorated_; }

    // Which titlebar gadgets a decorated window carries: close, zoom (toggle
    // full screen) and depth (send to back). Depth is on by default; a client
    // asks for the others (specs/window-manager.md). They sit at the
    // titlebar's right, close then zoom then depth.
    void set_gadgets(bool close, bool zoom, bool depth);

    void set_resizable(bool resizable) { resizable_ = resizable; }
    bool resizable() const { return resizable_; }

    void set_min_size(Size s) { min_size_ = s; }
    Size min_size() const { return min_size_; }

    void set_max_size(Size s) { max_size_ = s; }
    Size max_size() const { return max_size_; }

    void set_content(std::unique_ptr<Widget> content);
    Widget* content() const { return content_.get(); }

    void show();
    void hide();
    bool visible() const { return visible_; }
    void close();

    // Ask the console to focus this window, without raising it. A client that
    // wants to start focused calls this before exec; the request is kept
    // until the window exists (specs/window-manager.md).
    void request_focus();

    // Bureau integration
    uint64_t console_window_id() const { return console_window_id_; }
    uint64_t frame_window_id() const { return frame_window_id_; }

    // Callbacks
    std::function<void()> on_close_requested;
    std::function<void()> on_shown;
    std::function<void()> on_hidden;
    std::function<void(Rect)> on_moved_resized;
    std::function<void(bool)> on_focus_changed;

    // Called by Application when focus changes
    void on_focus_gained();
    void on_focus_lost();

    // Widget focus within the content tree. A pointer-down focuses the
    // focusable it lands in; Tab moves to the next (specs/trinket.md).
    void set_focus(Widget* widget);
    Widget* focused_widget() const { return focused_; }
    void focus_next();

    // Console event dispatch, called by Application with the packed word.
    void dispatch_key(uint64_t event);
    void dispatch_pointer(uint64_t event);

    // Request damage (repaint). Tier 1 repaints the whole window on any
    // damage: 480x360 is 172800 words and the event that caused it costs more
    // than the fill.
    void damage(const Rect& r = {});

    Application& application() { return app_; }

private:
    friend class Application;

    Application& app_;
    std::u32string title_;
    Rect rect_;
    bool decorated_ = true;
    bool resizable_ = true;
    bool visible_ = false;
    bool want_focus_ = false;
    Size min_size_ = {200, 150};
    Size max_size_ = {8192, 8192};
    std::unique_ptr<Widget> content_;

    // The window's backing: an offset into the application's console slice,
    // claimed once and reused.
    uint64_t backing_offset_ = ~0ull;

    // The region to repaint and to hand the console, frame-local. Empty means
    // the whole frame; one event's damages are unioned before the repaint.
    Rect damage_rect_;

    // The widget within the content tree that keys go to.
    Widget* focused_ = nullptr;

    // Console focus (the titlebar's active colour), and the titlebar drag and
    // the resize grip.
    bool active_ = false;
    bool dragging_ = false;
    int drag_offset_x_ = 0;
    int drag_offset_y_ = 0;
    bool resizing_ = false;
    int resize_origin_x_ = 0;  // the pointer's screen position when the resize began
    int resize_origin_y_ = 0;
    int resize_start_width_ = 0;
    int resize_start_height_ = 0;
    // Set while a geometry change updates the content's rectangle: the
    // damage it raises is collected, not repainted, because the console's
    // window is not the new size yet and a composite now would read the
    // backing at the wrong stride.
    bool geometry_change_ = false;

    // The titlebar gadgets, and the zoom's saved rectangle.
    bool gadget_close_ = false;
    bool gadget_zoom_ = false;
    bool gadget_depth_ = true;
    bool zoomed_ = false;
    Rect saved_rect_;

    // Bureau window IDs
    uint64_t console_window_id_ = 0;
    uint64_t frame_window_id_ = 0;

    // Internal
    int titlebar_height() const;
    int bottombar_height() const;
    Rect frame_for(const Rect& content) const;
    // Paint the frame into the backing and answer the region painted; repaint
    // also pushes that region to the console. A resize paints first and lets
    // the console's own resize composite it, so the backing is never read at
    // a stride it was not drawn at.
    Rect paint();
    // The gadget at `p` (frame-local): 0 none, 1 close, 2 zoom, 3 depth.
    int gadget_at(Point p) const;
    // The right-packed gadget slot, index 0 the rightmost.
    Rect gadget_rect(int index_from_right) const;
    // The Close gadget, at the titlebar's far left (specs/amiga-fidelity.md).
    Rect close_gadget_rect() const;
    // The resize gadget in the bottom bar's lower right.
    Rect resize_gadget_rect() const;
    void zoom();
    void create_bureau_window();
    void destroy_bureau_window();
    void update_bureau_window();
    void repaint();
    void register_menubar();
    void unregister_menubar();
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_WINDOW_H