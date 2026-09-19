/*
 * Trinket TextBox widget.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_TEXTBOX_H
#define AEGIR_TRINKET_TEXTBOX_H

#include <aegir/trinket/widget.h>
#include <aegir/trinket/font.h>
#include <aegir/trinket/unicode.h>
#include <functional>
#include <string>

namespace aegir::trinket {

class TextBox : public Widget {
public:
    TextBox();
    ~TextBox() override;

    void set_text(std::u32string_view text);
    void set_text(std::string_view text);  // UTF-8
    std::u32string text() const { return text_; }
    std::string text_utf8() const;

    void set_placeholder(std::u32string_view text);
    void set_placeholder(std::string_view text);
    std::u32string placeholder() const { return placeholder_; }

    void set_max_length(size_t len) { max_length_ = len; }
    void set_password_mode(bool enable) { password_mode_ = enable; damage(); }
    void set_read_only(bool ro) { read_only_ = ro; }
    void set_multiline(bool ml) { multiline_ = ml; damage(); }

    // Cursor position (in codepoints)
    size_t cursor_pos() const { return cursor_; }
    void set_cursor_pos(size_t pos);

    // Selection
    bool has_selection() const { return selection_start_ != selection_end_; }
    size_t selection_start() const { return std::min(selection_start_, selection_end_); }
    size_t selection_end() const { return std::max(selection_start_, selection_end_); }
    void set_selection(size_t start, size_t end);
    void clear_selection() { selection_start_ = selection_end_ = cursor_; }
    std::u32string selected_text() const;

    void set_font(Font* f) { font_ = f; damage(); }
    Font* font() const { return font_; }

    // Callbacks
    std::function<void(std::u32string_view)> on_text_changed;
    std::function<void()> on_submit;       // Enter pressed
    std::function<void()> on_cancel;       // Escape pressed

    // Scrolling for multiline
    void ensure_cursor_visible();
    int h_scroll() const { return h_scroll_; }
    int v_scroll() const { return v_scroll_; }

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_mouse_down(const MouseEvent& event) override;
    void on_mouse_up(const MouseEvent& event) override;
    void on_mouse_move(const MouseEvent& event) override;
    void on_key_down(const KeyEvent& event) override;
    void on_key_up(const KeyEvent& event) override;
    void on_focus_gained() override;
    void on_focus_lost() override;
    Size preferred_size() const override;

private:
    std::u32string text_;
    std::u32string placeholder_;
    size_t cursor_ = 0;
    size_t selection_start_ = 0;
    size_t selection_end_ = 0;
    size_t max_length_ = 0;
    bool password_mode_ = false;
    bool read_only_ = false;
    bool multiline_ = false;
    Font* font_ = nullptr;
    int h_scroll_ = 0;
    int v_scroll_ = 0;
    uint64_t blink_timer_ = 0;
    bool cursor_visible_ = true;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_TEXTBOX_H