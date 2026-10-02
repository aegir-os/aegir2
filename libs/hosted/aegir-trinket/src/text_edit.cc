/*
 * Trinket TextEdit implementation (specs/trinket/editor.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/text_edit.h>

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/font.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>

#include <algorithm>

namespace aegir::trinket {

namespace {
/* The margin between the host's frame and the first cell, as the terminal's:
 * glyphs at column 0 would be cut by a frame drawn over the edge. */
constexpr int kTextPadding = 4;
} // namespace

TextEdit::TextEdit() {
    /* The document's own changes -- an insert, a backspace -- reach the view
     * through it, so the widget routes them through one place. */
    document_.on_change = [this]() { caret_moved(); };
}

TextEdit::~TextEdit() = default;

void TextEdit::set_text(std::u32string_view text) {
    document_.set_text(text);
    first_row_ = 0;
    h_scroll_ = 0;
    caret_moved();
}

void TextEdit::set_text(std::string_view text) {
    set_text(utf8_to_utf32(text));
}

void TextEdit::set_font(Font* font) {
    font_ = font;
    on_layout();
    damage();
}

void TextEdit::set_wrap(bool wrap) {
    if (wrap_ == wrap) return;
    wrap_ = wrap;
    h_scroll_ = 0;
    caret_moved();
}

void TextEdit::set_colors(Color text, Color background) {
    text_color_ = text;
    background_ = background;
    colors_set_ = true;
    damage();
}

Font* TextEdit::active_font() const {
    return font_ != nullptr ? font_ : Application::instance()->default_font();
}

int TextEdit::advance() const {
    Font* const font = active_font();
    if (font == nullptr) return 0;
    Glyph const* const glyph = font->glyph(U'M');
    if (glyph != nullptr && glyph->advance > 0) return glyph->advance;
    return std::max(1, font->height() / 2);
}

int TextEdit::line_height() const {
    Font* const font = active_font();
    return font == nullptr ? 0 : font->height();
}

int TextEdit::view_columns() const {
    int const cell = advance();
    if (cell <= 0) return 1;
    return std::max(1, (rect_.width - 2 * kTextPadding) / cell);
}

int TextEdit::rows_of(int line) const {
    if (!wrap_) return 1;
    int const length = std::max(1, document_.line_length(line));
    return (length + view_columns() - 1) / view_columns();
}

int TextEdit::row_count() const {
    int rows = 0;
    for (int line = 0; line < document_.line_count(); ++line) {
        rows += rows_of(line);
    }
    return rows;
}

int TextEdit::visible_rows() const {
    int const height = line_height();
    if (height <= 0) return 1;
    return std::max(1, (rect_.height - 2 * kTextPadding) / height);
}

TextEdit::Row TextEdit::row_at(int row) const {
    Row found;
    int left = row;
    for (int line = 0; line < document_.line_count(); ++line) {
        int const rows = rows_of(line);
        if (left < rows) {
            found.line = line;
            found.column = left * (wrap_ ? view_columns() : 1);
            return found;
        }
        left -= rows;
    }
    /* Past the end: the last line's start, so a clamp never reads out of it. */
    found.line = std::max(0, document_.line_count() - 1);
    found.column = 0;
    return found;
}

int TextEdit::row_of(int line, int column) const {
    int row = 0;
    for (int i = 0; i < line && i < document_.line_count(); ++i) {
        row += rows_of(i);
    }
    if (wrap_) row += column / view_columns();
    return row;
}

int TextEdit::caret_x(int line, int column) const {
    Font* const font = active_font();
    if (font == nullptr) return 0;
    int const start = wrap_ ? (column / view_columns()) * view_columns() : 0;
    std::u32string const& text = document_.line(line);
    int const from = std::min(start, static_cast<int>(text.size()));
    int const to = std::min(column, static_cast<int>(text.size()));
    return font->measure(std::u32string_view(text).substr(from, to - from)).width -
           (wrap_ ? 0 : h_scroll_);
}

void TextEdit::set_first_row(int row) {
    int const most = std::max(0, row_count() - visible_rows());
    int const clamped = std::max(0, std::min(row, most));
    if (clamped == first_row_) return;
    first_row_ = clamped;
    damage();
}

void TextEdit::scroll_by(int rows) {
    set_first_row(first_row_ + rows);
}

void TextEdit::ensure_cursor_visible() {
    if (active_font() == nullptr) return;
    TextDocument::Cursor const cursor = document_.cursor();
    int const row = row_of(cursor.line, cursor.column);
    if (row < first_row_) {
        first_row_ = row;
    } else if (row >= first_row_ + visible_rows()) {
        first_row_ = row - visible_rows() + 1;
    }
    int const most = std::max(0, row_count() - visible_rows());
    first_row_ = std::max(0, std::min(first_row_, most));

    if (wrap_) {
        h_scroll_ = 0;
        return;
    }
    /* The clip follows the caret sideways. `caret_x` is already the on-screen
     * offset, so the caret's own text-space x is that plus the scroll; past the
     * right edge the view scrolls out, before the left edge it scrolls back. */
    int const text_x = caret_x(cursor.line, cursor.column) + h_scroll_;
    int const cell = advance();
    int const width = std::max(0, rect_.width - 2 * kTextPadding);
    int const on_screen = text_x - h_scroll_;
    if (on_screen < 0) {
        h_scroll_ = std::max(0, text_x);
    } else if (on_screen + cell > width) {
        h_scroll_ = std::max(0, text_x + cell - width);
    }
}

void TextEdit::on_layout() {
    int const rows = row_count();
    int const most = std::max(0, rows - visible_rows());
    first_row_ = std::max(0, std::min(first_row_, most));
    if (h_scroll_ < 0) h_scroll_ = 0;
}

void TextEdit::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);
    Theme& theme = Application::instance()->theme();
    Font* const font = active_font();
    Color const background = colors_set_ ? background_ : theme.color(ColorRole::INPUT_BG);
    Color const text = colors_set_ ? text_color_ : theme.color(ColorRole::TEXT);
    canvas.fill_rect(rect_, background);
    if (font == nullptr) return;

    int const height = line_height();
    int const rows = visible_rows();
    Rect const inner{rect_.x + kTextPadding, rect_.y + kTextPadding,
                     std::max(0, rect_.width - 2 * kTextPadding),
                     std::max(0, rect_.height - 2 * kTextPadding)};
    for (int i = 0; i < rows; ++i) {
        int const row = first_row_ + i;
        if (row >= row_count()) break;
        Row const at = row_at(row);
        std::u32string const& line = document_.line(at.line);
        int const from = std::min(at.column, static_cast<int>(line.size()));
        int const count = wrap_ ? view_columns() : static_cast<int>(line.size()) - from;
        std::u32string_view const piece =
            std::u32string_view(line).substr(from, static_cast<size_t>(std::max(0, count)));
        int const x = inner.x - (wrap_ ? 0 : h_scroll_);
        int const y = inner.y + i * height;
        canvas.draw_text_clipped(inner, {x, y}, piece, font, text);
    }

    if (!focused_) return;
    TextDocument::Cursor const cursor = document_.cursor();
    int const row = row_of(cursor.line, cursor.column);
    if (row < first_row_ || row >= first_row_ + rows) return;
    std::u32string const& line = document_.line(cursor.line);
    int const x = inner.x + caret_x(cursor.line, cursor.column);
    int const y = inner.y + (row - first_row_) * height;
    Rect const cell{x, y, std::max(1, advance()), height};
    Theme::CursorShape const shape = document_.mode() == TextDocument::Mode::INSERT
                                         ? Theme::CursorShape::BLOCK
                                         : Theme::CursorShape::UNDERLINE;
    theme.draw_cursor(canvas, cell, shape);
    if (shape == Theme::CursorShape::BLOCK) {
        /* A block inverts the cell: the glyph under it is drawn in the cursor's
         * own text colour (specs/trinket/editor.md). */
        char32_t const under = cursor.column < static_cast<int>(line.size())
                                   ? line[static_cast<size_t>(cursor.column)]
                                   : U' ';
        canvas.draw_text_clipped(inner, {x, y},
                                 std::u32string_view(&under, 1), font,
                                 theme.color(ColorRole::CURSOR_TEXT));
    }
}

void TextEdit::move_rows(int rows) {
    TextDocument::Cursor const cursor = document_.cursor();
    int const target = std::max(
        0, std::min(row_of(cursor.line, cursor.column) + rows, row_count() - 1));
    Row const at = row_at(target);
    /* Keep the cell the caret held on its row, clamped to the new one. */
    int const want = cursor.column % view_columns();
    document_.set_cursor(at.line, at.column + want);
}

void TextEdit::on_key_down(KeyEvent const& event) {
    if (!enabled_) return;
    bool moved = false;
    switch (event.code) {
    case KeyCode::LEFT:
        document_.move(-1, 0);
        moved = true;
        break;
    case KeyCode::RIGHT:
        document_.move(1, 0);
        moved = true;
        break;
    case KeyCode::UP:
        if (wrap_) {
            move_rows(-1);
        } else {
            document_.move(0, -1);
        }
        moved = true;
        break;
    case KeyCode::DOWN:
        if (wrap_) {
            move_rows(1);
        } else {
            document_.move(0, 1);
        }
        moved = true;
        break;
    case KeyCode::HOME:
        document_.home();
        moved = true;
        break;
    case KeyCode::END:
        document_.end();
        moved = true;
        break;
    case KeyCode::PAGE_UP:
        document_.move(0, -(visible_rows() - 1));
        moved = true;
        break;
    case KeyCode::PAGE_DOWN:
        document_.move(0, visible_rows() - 1);
        moved = true;
        break;
    case KeyCode::INSERT:
        document_.toggle_mode();
        caret_moved();
        return;
    case KeyCode::BACKSPACE:
        document_.backspace();
        return;
    case KeyCode::DELETE_KEY:
        document_.erase();
        return;
    case KeyCode::ENTER:
        document_.newline();
        return;
    default:
        if (event.text >= 32 && event.text != 127) {
            document_.insert(event.text);
        }
        return;
    }
    if (moved) caret_moved();
}

void TextEdit::clicked(Point pos) {
    Font* const font = active_font();
    if (font == nullptr) return;
    int const height = std::max(1, line_height());
    int const row = std::max(0, first_row_ + (pos.y - rect_.y - kTextPadding) / height);
    Row const at = row_at(row);
    std::u32string const& line = document_.line(at.line);
    int const x = pos.x - rect_.x - kTextPadding + (wrap_ ? 0 : h_scroll_);
    /* The nearest cell boundary: walk the piece until the pointer is past a
     * cell's middle. */
    int column = at.column;
    int width = 0;
    int const limit = wrap_ ? std::min(static_cast<int>(line.size()),
                                       at.column + view_columns())
                            : static_cast<int>(line.size());
    while (column < limit) {
        int const cell = font->measure(
                             std::u32string_view(line).substr(static_cast<size_t>(column), 1))
                             .width;
        if (x < width + cell / 2) break;
        width += cell;
        ++column;
    }
    document_.set_cursor(at.line, column);
    caret_moved();
}

void TextEdit::on_mouse_down(MouseEvent const& event) {
    if (!enabled_ || event.button != MouseButton::LEFT) return;
    set_focused(true);
    clicked(event.pos);
}

void TextEdit::changed() {
    damage();
    if (on_change) on_change();
}

void TextEdit::caret_moved() {
    ensure_cursor_visible();
    changed();
}

Size TextEdit::preferred_size() const {
    int const cell = std::max(1, advance());
    int const height = std::max(1, line_height());
    return {cell * 40 + 2 * kTextPadding, height * 10 + 2 * kTextPadding};
}

Size TextEdit::minimum_size() const {
    int const cell = std::max(1, advance());
    int const height = std::max(1, line_height());
    return {cell * 4 + 2 * kTextPadding, height + 2 * kTextPadding};
}

Size TextEdit::maximum_size() const {
    /* The editor fills its host: it lays itself out from whatever rectangle it
     * is given (specs/trinket/layout.md). */
    int const big = std::numeric_limits<int>::max();
    return {big, big};
}

} // namespace aegir::trinket
