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
    Size min_size_ = {200, 150};
    Size max_size_ = {8192, 8192};
    std::unique_ptr<Widget> content_;

    // The window's backing: an offset into the application's console slice,
    // claimed once and reused.
    uint64_t backing_offset_ = ~0ull;

    // The widget within the content tree that keys go to.
    Widget* focused_ = nullptr;

    // Console focus (the titlebar's active colour), and the titlebar drag and
    // the resize grip.
    bool active_ = false;
    bool dragging_ = false;
    int drag_offset_x_ = 0;
    int drag_offset_y_ = 0;
    bool resizing_ = false;
    int resize_offset_x_ = 0;
    int resize_offset_y_ = 0;

    // Bureau window IDs
    uint64_t console_window_id_ = 0;
    uint64_t frame_window_id_ = 0;

    // Internal
    int titlebar_height() const;
    Rect frame_for(const Rect& content) const;
    void create_bureau_window();
    void destroy_bureau_window();
    void update_bureau_window();
    void repaint();
    void register_menubar();
    void unregister_menubar();
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_WINDOW_H