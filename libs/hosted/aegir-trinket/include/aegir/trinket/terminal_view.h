/*
 * TerminalView: the widget that draws a TerminalBuffer (specs/terminal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A monospace cell grid: the buffer is the model, this is the pixels. The
 * `CON:` handler owns one and feeds it a client's stream; the toolkit's
 * demo shows one directly.
 */

#ifndef AEGIR_TRINKET_TERMINAL_VIEW_H
#define AEGIR_TRINKET_TERMINAL_VIEW_H

#include <aegir/trinket/terminal_buffer.h>
#include <aegir/trinket/widget.h>
#include <cstdint>

namespace aegir::trinket {

class Font;
class Canvas;

class TerminalView : public Widget {
public:
    TerminalView();
    TerminalView(int columns, int rows);
    ~TerminalView() override;

    TerminalBuffer& buffer() { return buffer_; }
    TerminalBuffer const& buffer() const { return buffer_; }

    void set_font(Font* font);
    Font* font() const { return font_; }

    void set_colors(Color text, Color background);
    Color text_color() const { return text_color_; }
    Color background_color() const override { return background_; }

    /* The grid metrics, from the font: one cell is its advance by its height.
     * Zero when no font can be loaded. */
    int cell_advance() const;
    int cell_height() const;

    bool focusable() const override { return true; }
    Size preferred_size() const override;

    /* Whether the cursor is drawn (the handler shows it while a line is
     * being edited). */
    void set_cursor_visible(bool visible) { cursor_visible_ = visible; }
    bool cursor_visible() const { return cursor_visible_; }

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_key_down(const KeyEvent& event) override;
    void on_layout() override;

private:
    Font* font_ = nullptr;
    TerminalBuffer buffer_;
    Color text_color_{0xD0u, 0xD0u, 0xD0u};
    Color background_{0x00u, 0x00u, 0x00u};
    bool cursor_visible_ = true;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_TERMINAL_VIEW_H
