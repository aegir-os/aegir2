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

namespace {
/* The margin between the window's frame and the first cell: the frame is drawn
 * over the content's edge, so glyphs at cell 0 would be cut by it. Four pixels
 * clears the frame and reads as a terminal's border. */
constexpr int kTextPadding = 4;
}  // namespace

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
    return {buffer_.columns() * advance + 2 * kTextPadding,
            buffer_.rows() * height + 2 * kTextPadding};
}

void TerminalView::on_layout() {
    /* Before the window lays the content out the rect is empty, and a grid
     * sized to it is one column wide. A client that fills the buffer before
     * show() -- the demo's grid does -- would then wrap every character into
     * its own line. No rect, no resize. */
    if (rect_.width <= 0 || rect_.height <= 0) return;
    int const advance = cell_advance();
    int const height = cell_height();
    if (advance <= 0 || height <= 0) return;
    int const columns = std::max(1, (rect_.width - 2 * kTextPadding) / advance);
    int const rows = std::max(1, (rect_.height - 2 * kTextPadding) / height);
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
    /* The display order is derived from the text; derive it only when the text
     * or the viewport changed, not on every repaint of the same grid. The row
     * buffers are reused, so a keystroke that changes one line does not
     * allocate a fresh vector per visible line. */
    if (buffer_.version() != cached_version_ || first != cached_first_) {
        cached_version_ = buffer_.version();
        cached_first_ = first;
        int rows = 0;
        for (int row = 0; row < buffer_.rows(); ++row) {
            int const index = first + row;
            if (index >= buffer_.line_count()) break;
            if (static_cast<int>(cached_visual_.size()) <= rows) {
                cached_visual_.emplace_back();
            }
            buffer_.visual_cells_into(index, cached_visual_[static_cast<std::size_t>(rows)]);
            ++rows;
        }
        cached_rows_ = rows;
    }
    for (int row = 0; row < cached_rows_; ++row) {
        int x = rect_.x + kTextPadding;
        int const y = rect_.y + kTextPadding + row * height;
        for (TerminalCell const& cell : cached_visual_[static_cast<std::size_t>(row)]) {
            /* A blank cell is the background already filled; a combining mark
             * (width 0) draws at the running x and does not advance; a wide
             * cell draws once and advances two. */
            if (!(cell.cp == U' ' && cell.width == 1)) {
                char32_t const one_char = cell.cp;
                canvas.draw_text({x, y}, std::u32string_view(&one_char, 1), font,
                                 text_color_);
            }
            x += cell.width * advance;
        }
    }

    if (cursor_visible_ && focused_) {
        int const line = buffer_.cursor_line();
        if (line >= first && line < first + buffer_.rows()) {
            int const column = buffer_.visual_column(line, buffer_.cursor_cell());
            int const x = rect_.x + kTextPadding + column * advance;
            int const y = rect_.y + kTextPadding + (line - first) * height;
            canvas.fill_rect({x, y, advance, height}, text_color_);
        }
    }
}

void TerminalView::on_key_down(const KeyEvent& event) {
    if (on_key && on_key(event)) {
        /* The editor wrote the line into the buffer; the view is what draws it,
         * so a key that changed the line damages the view. */
        damage();
        return;
    }
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
