/*
 * Trinket TextEdit: the multiline editor's view (specs/trinket/editor.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The view over a TextDocument: it renders the lines, scrolls, and routes keys
 * and pointer to the document's cursor. Insert mode draws the theme's block
 * cursor and overwrite its underline. Longer than the view, a line is clipped
 * and the view scrolls sideways (the default); `set_wrap` soft-wraps it
 * instead, and then the view scrolls only down.
 */

#ifndef AEGIR_TRINKET_TEXT_EDIT_H
#define AEGIR_TRINKET_TEXT_EDIT_H

#include <aegir/trinket/color.h>
#include <aegir/trinket/text_document.h>
#include <aegir/trinket/widget.h>

#include <functional>
#include <limits>
#include <string>

namespace aegir::trinket {

class Font;

class TextEdit : public Widget {
public:
    TextEdit();
    ~TextEdit() override;

    TextDocument& document() { return document_; }
    TextDocument const& document() const { return document_; }

    void set_text(std::u32string_view text);
    void set_text(std::string_view text); // UTF-8
    std::u32string text() const { return document_.text(); }

    void set_font(Font* font);
    Font* font() const { return font_; }

    /* Soft-wrap a line wider than the view. Off (the default): the line is
     * clipped at the right edge and the view scrolls sideways to follow the
     * cursor, as the terminal clips. On: the line is broken at the view's width,
     * as Amiga's Ed breaks it, and the view scrolls only down. */
    void set_wrap(bool wrap);
    bool wrap() const { return wrap_; }

    void set_colors(Color text, Color background);

    /* The scroll state a client wires its Scrollbar to, as the terminal's buffer
     * exposes its own (specs/trinket/scrollbar.md). A row is a line when
     * clipping and a soft-wrapped piece of one when wrapping. */
    int row_count() const;
    int visible_rows() const;
    int first_row() const { return first_row_; }
    void set_first_row(int row);
    void scroll_by(int rows);

    /* Move the scroll so the caret shows, and the caret with the keys. */
    void ensure_cursor_visible();

    bool focusable() const override { return true; }

    /* The text, the caret or the scroll changed: the client repaints and
     * re-syncs its scrollbar. */
    std::function<void()> on_change;

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_mouse_down(const MouseEvent& event) override;
    void on_key_down(const KeyEvent& event) override;
    void on_layout() override;
    Size preferred_size() const override;
    Size minimum_size() const override;
    Size maximum_size() const override;

private:
    /* A row of the view: which line it shows and the column it starts at. */
    struct Row {
        int line = 0;
        int column = 0;
    };

    Font* active_font() const;
    int advance() const;      /* one cell's width */
    int line_height() const;  /* one row's height */
    int view_columns() const; /* the cells a row holds, for wrapping */
    int rows_of(int line) const;      /* the rows a line occupies */
    Row row_at(int row) const;        /* the first row's line/column */
    int row_of(int line, int column) const;
    int caret_x(int line, int column) const; /* from the view's left edge */

    void move_rows(int rows);   /* Up/Down: by view row when wrapping */
    void changed();
    void caret_moved();
    void clicked(Point pos);

    TextDocument document_;
    Font* font_ = nullptr;
    Color text_color_{};
    Color background_{};
    bool wrap_ = false;
    int first_row_ = 0;
    int h_scroll_ = 0; /* the clip's horizontal offset, in pixels */
    bool colors_set_ = false;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_TEXT_EDIT_H
