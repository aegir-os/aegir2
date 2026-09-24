/*
 * TerminalView implementation (specs/terminal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/terminal_view.h>

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/font.h>

#include <algorithm>

namespace aegir::trinket {

TerminalView::TerminalView() = default;
TerminalView::TerminalView(int columns, int rows) : buffer_(columns, rows) {}
TerminalView::~TerminalView() = default;

void TerminalView::set_font(Font* font) {
    font_ = font;
    on_layout();
    damage();
}

void TerminalView::set_colors(Color text, Color background) {
    text_color_ = text;
    background_ = background;
    damage();
}

int TerminalView::cell_advance() const {
    Font* const font = font_ ? font_ : Application::instance()->default_font();
    if (font == nullptr) return 0;
    Glyph const* const glyph = font->glyph(U'M');
    if (glyph != nullptr && glyph->advance > 0) return glyph->advance;
    return std::max(1, font->height() / 2);
}

int TerminalView::cell_height() const {
    Font* const font = font_ ? font_ : Application::instance()->default_font();
    return font == nullptr ? 0 : font->height();
}

Size TerminalView::preferred_size() const {
    int const advance = cell_advance();
    int const height = cell_height();
    return {buffer_.columns() * advance, buffer_.rows() * height};
}

void TerminalView::on_layout() {
    int const advance = cell_advance();
    int const height = cell_height();
    if (advance <= 0 || height <= 0) return;
    int const columns = std::max(1, rect_.width / advance);
    int const rows = std::max(1, rect_.height / height);
    if (columns != buffer_.columns() || rows != buffer_.rows()) {
        buffer_.resize(columns, rows);
    }
}

void TerminalView::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);

    canvas.fill_rect(rect_, background_);

    Font* const font = font_ ? font_ : Application::instance()->default_font();
    if (font == nullptr) return;

    int const advance = cell_advance();
    int const height = font->height();
    int const first = buffer_.visible_first_line();
    for (int row = 0; row < buffer_.rows(); ++row) {
        int const index = first + row;
        if (index >= buffer_.line_count()) break;
        int x = rect_.x;
        int const y = rect_.y + row * height;
        for (TerminalCell const& cell : buffer_.visual_cells(index)) {
            /* A blank cell is the background already filled; a combining mark
             * (width 0) draws at the running x and does not advance; a wide
             * cell draws once and advances two. */
            if (!(cell.cp == U' ' && cell.width == 1)) {
                std::u32string const one(1, cell.cp);
                canvas.draw_text({x, y}, one, font, text_color_);
            }
            x += cell.width * advance;
        }
    }

    if (cursor_visible_ && focused_) {
        int const line = buffer_.cursor_line();
        if (line >= first && line < first + buffer_.rows()) {
            int const column = buffer_.visual_column(line, buffer_.cursor_cell());
            int const x = rect_.x + column * advance;
            int const y = rect_.y + (line - first) * height;
            canvas.fill_rect({x, y, advance, height}, text_color_);
        }
    }
}

void TerminalView::on_key_down(const KeyEvent& event) {
    switch (event.code) {
    case KeyCode::PAGE_UP:
        buffer_.scroll_by(buffer_.rows());
        damage();
        break;
    case KeyCode::PAGE_DOWN:
        buffer_.scroll_by(-buffer_.rows());
        damage();
        break;
    case KeyCode::HOME:
        buffer_.scroll_by(buffer_.scrollback_lines());
        damage();
        break;
    case KeyCode::END:
        buffer_.scroll_to_bottom();
        damage();
        break;
    default:
        break;
    }
}

} // namespace aegir::trinket
