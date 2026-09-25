/*
 * The terminal's text model: a cell grid with scrollback, cursor and line
 * discipline (specs/terminal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This is the part of the terminal that is a value, not a widget: it knows
 * nothing of pixels, events or seL4, so the build can compile it for the host
 * and the conformance harness can assert its behaviour exactly
 * (scripts/terminal_conformance.cc). `TerminalView` is the widget that renders
 * one; the `CON:` handler (a later arc) drives one from a client's stream.
 */

#ifndef AEGIR_TRINKET_TERMINAL_BUFFER_H
#define AEGIR_TRINKET_TERMINAL_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace aegir::trinket {

/* One grid cell: a code point and the number of columns it occupies. A
 * combining mark is width 0 and stays in the line after its base; a wide
 * character is width 2. The line's cell count is not its column count. */
struct TerminalCell {
    char32_t cp = U' ';
    uint8_t width = 1;
};

using TerminalLine = std::vector<TerminalCell>;

/* A monospace text grid. Writes arrive as text and land at the cursor; the
 * cursor moves on `\n`, `\r`, `\b` and `\t`, and wraps at the right edge.
 * Lines that leave the top go to scrollback. */
class TerminalBuffer {
public:
    TerminalBuffer(int columns = 80, int rows = 24);

    int columns() const { return columns_; }
    int rows() const { return rows_; }
    void resize(int columns, int rows);

    /* Output. UTF-8 bytes are decoded a code point at a time. */
    void write(std::string_view utf8);
    void write(std::u32string_view text);

    /* The movement and editing a character stream understands. */
    void newline();
    void carriage_return();
    void backspace();
    void tab();
    void erase_to_end_of_line();
    void clear();

    /* The cursor, in the buffer's own coordinates: a line index into `lines`
     * and a cell index into that line. */
    int cursor_line() const { return cursor_line_; }
    int cursor_cell() const { return cursor_cell_; }
    void set_cursor(int line, int cell);
    /* The cursor's column among the line's cells (widths summed). */
    int cursor_column() const;

    /* Content. `line()` is the logical code points of a line, combining marks
     * included; `visual_line()` is the same text reordered for display by
     * UAX #9, and `visual_column()` maps a logical cell to its display
     * column. */
    int line_count() const { return static_cast<int>(lines_.size()); }
    std::u32string line(int index) const;
    int line_columns(int index) const;
    std::u32string visual_line(int index) const;
    /* The line's cells in display order, mirrored, each with its width -- what
     * a renderer walks to place a wide character in two columns and a
     * combining mark in none. */
    std::vector<TerminalCell> visual_cells(int index) const;
    /* The same, filled into `out` with its capacity reused, for a renderer that
     * keeps its own buffers across paints: the return-by-value form allocates a
     * fresh vector per visible line on every keystroke. */
    void visual_cells_into(int index, std::vector<TerminalCell>& out) const;
    int visual_column(int index, int cell) const;
    /* Every line joined by `\n`, for a whole-buffer assertion. */
    std::u32string text() const;

    /* Scrollback. A budget of zero is unbounded -- capacity grows on demand --
     * and a positive budget is the most the ring may hold, oldest lines
     * dropped first (project rule: no arbitrary limits). */
    std::size_t scrollback_budget() const { return scrollback_budget_; }
    void set_scrollback_budget(std::size_t bytes) { scrollback_budget_ = bytes; }
    /* How many lines may be retained behind the viewport, for a scroll bar. */
    int scrollback_lines() const;
    int scroll_offset() const { return scroll_offset_; }
    void scroll_by(int lines);
    void scroll_to_bottom();
    /* The line index drawn at the top of a `rows`-tall viewport. */
    int visible_first_line() const;

    /* Bumped by every content change, so a renderer can cache what it derived
     * from the cells (the BiDi order a paint walks) and recompute only when the
     * text actually changed -- a repaint of unchanged text is common. */
    uint64_t version() const { return version_; }

private:
    TerminalLine& current_line();
    void ensure_line(int index);
    void apply(char32_t cp);
    void put(char32_t cp);
    void enforce_budget();
    std::size_t retained_bytes() const;

    int columns_;
    int rows_;
    std::deque<TerminalLine> lines_;
    int cursor_line_ = 0;
    int cursor_cell_ = 0;
    std::size_t scrollback_budget_ = 0;
    int scroll_offset_ = 0;
    uint64_t version_ = 0;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_TERMINAL_BUFFER_H
