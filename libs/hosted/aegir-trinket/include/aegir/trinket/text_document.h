/*
 * Trinket TextDocument: the editable text a TextEdit shows (specs/trinket/editor.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The editor's model: its lines, the cursor, the mode (insert or overwrite) and
 * the edit operations. It is pure -- no font, no theme, no rectangle -- so the
 * host check pins the arithmetic (scripts/check_text_document.py). The widget
 * renders it and routes keys to it; the document owns no pixels.
 */

#ifndef AEGIR_TRINKET_TEXT_DOCUMENT_H
#define AEGIR_TRINKET_TEXT_DOCUMENT_H

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aegir::trinket {

class TextDocument {
public:
    /* Insert shifts the tail right; overwrite replaces the cell under the
     * cursor, and appends at a line's end (there is no cell to replace). */
    enum class Mode { INSERT, OVERWRITE };

    /* A caret between two cells: `line` indexes the lines, `column` the
     * codepoints before the caret, so column 0 is the line's head and
     * `line_length` its end. */
    struct Cursor {
        int line = 0;
        int column = 0;
    };

    TextDocument() : lines_(1) {}
    explicit TextDocument(std::u32string_view text);

    /* The text as one string, its lines joined by '\n'. Setting it splits on
     * '\n' and puts the cursor at the head. */
    void set_text(std::u32string_view text);
    std::u32string text() const;

    int line_count() const { return static_cast<int>(lines_.size()); }
    std::u32string const& line(int row) const;
    int line_length(int row) const;

    Cursor cursor() const { return cursor_; }
    void set_cursor(Cursor c);
    void set_cursor(int line, int column) { set_cursor(Cursor{line, column}); }

    Mode mode() const { return mode_; }
    void set_mode(Mode m) { mode_ = m; }
    void toggle_mode() {
        mode_ = mode_ == Mode::INSERT ? Mode::OVERWRITE : Mode::INSERT;
    }

    /* The edits. Each marks the document dirty and calls `on_change` when it
     * changed anything. */
    void insert(char32_t cp);
    void newline();
    void backspace();
    void erase();

    /* Move the caret: `columns` by cells on the line, `lines` by lines. Vertical
     * movement aims for the column the caret last held on its own, so walking up
     * and down a ragged block comes back to where it started. */
    void move(int columns, int lines);
    void home();
    void end();

    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    /* The view repaints and rescrolls on it; the document does neither. */
    std::function<void()> on_change;

private:
    void clamp_cursor();
    void changed();

    std::vector<std::u32string> lines_;
    Cursor cursor_{};
    int want_column_ = 0; /* the column vertical movement aims for */
    Mode mode_ = Mode::INSERT;
    bool dirty_ = false;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_TEXT_DOCUMENT_H
