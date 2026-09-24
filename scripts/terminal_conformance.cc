/*
 * Host conformance for the terminal's text grid.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * scripts/check_terminal.py compiles this with the host compiler against
 * terminal_buffer.cc, bidi.cc, unicode.cc and the generated Unicode tables and
 * runs it; it is not part of any target build. The grid is a pure value, so
 * cell placement, wrapping, scrollback, wide and combining characters, and
 * BiDi reordering and cursor mapping are asserted exactly (specs/terminal.md).
 */

#include <aegir/trinket/terminal_buffer.h>
#include <aegir/trinket/unicode.h>

#include <cstdio>
#include <string>
#include <cstdint>

namespace {

using aegir::trinket::TerminalBuffer;
using aegir::trinket::TerminalCell;
using aegir::trinket::utf32_to_utf8;

int g_checks = 0;
int g_failures = 0;

void expect_text(std::u32string const &got, std::u32string const &want, char const *what)
{
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::fprintf(stderr, "FAIL  %s: got \"%s\" want \"%s\"\n", what,
                     utf32_to_utf8(got).c_str(), utf32_to_utf8(want).c_str());
    }
}

void expect_int(int got, int want, char const *what)
{
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::fprintf(stderr, "FAIL  %s: got %d want %d\n", what, got, want);
    }
}

/* "abc", the simple append. */
void check_append()
{
    TerminalBuffer b(20, 5);
    b.write("abc");
    expect_text(b.text(), U"abc", "append: the line is what was written");
    expect_int(b.cursor_column(), 3, "append: the cursor advanced by the cells");
}

/* "\n" opens a line; the cursor lands at its start. */
void check_newline()
{
    TerminalBuffer b(20, 5);
    b.write("abc\nXY");
    expect_int(b.line_count(), 2, "newline: a second line");
    expect_text(b.line(0), U"abc", "newline: the first line");
    expect_text(b.line(1), U"XY", "newline: the second line");
    expect_int(b.cursor_line(), 1, "newline: the cursor is on the second line");
    expect_int(b.cursor_column(), 2, "newline: the cursor advanced past XY");
}

/* "\r" returns to the start; writing overwrites. */
void check_carriage_return()
{
    TerminalBuffer b(20, 5);
    b.write("abc\rx");
    expect_text(b.line(0), U"xbc", "carriage return: the write overwrote at column 0");
}

/* "\b" moves the cursor left; writing overwrites there. */
void check_backspace()
{
    TerminalBuffer b(20, 5);
    b.write("abc\b\bX");
    expect_text(b.line(0), U"aXc", "backspace: two back and a write overwrote");
}

/* A wide character occupies two columns but is one cell. */
void check_wide()
{
    TerminalBuffer b(20, 5);
    b.write("\xE6\x97\xA5\xE6\x9C\xAC"); /* U+65E5 U+672C */
    expect_text(b.line(0), U"\u65E5\u672C", "wide: the code points are in the line");
    expect_int(b.line_columns(0), 4, "wide: two characters occupy four columns");
    expect_int(b.cursor_column(), 4, "wide: the cursor advanced two cells' columns");
    std::vector<TerminalCell> const cells = b.visual_cells(0);
    expect_int(static_cast<int>(cells.size()), 2, "wide: one cell per code point");
    expect_int(static_cast<int>(cells[0].width), 2, "wide: the cell is two columns");
}

/* A combining mark takes no column and stays after its base. */
void check_combining()
{
    TerminalBuffer b(20, 5);
    b.write("e");
    b.write("\xCC\x81"); /* U+0301 COMBINING ACUTE */
    expect_text(b.line(0), U"e\u0301", "combining: the mark stays in the line");
    expect_int(b.line_columns(0), 1, "combining: the mark adds no column");
    expect_int(b.cursor_column(), 1, "combining: the cursor did not advance");
}

/* "\t" advances to the next multiple of eight. */
void check_tab()
{
    TerminalBuffer b(20, 5);
    b.write("a\tb");
    expect_int(b.line_columns(0), 9, "tab: a, seven spaces, then b");
    expect_int(b.cursor_column(), 9, "tab: the cursor is past the b");
}

/* Writing past the right edge wraps to the next line. */
void check_wrap()
{
    TerminalBuffer b(4, 5);
    b.write("abcdef");
    expect_int(b.line_count(), 2, "wrap: two lines");
    expect_text(b.line(0), U"abcd", "wrap: the first line fills the width");
    expect_text(b.line(1), U"ef", "wrap: the rest is on the second");
}

/* erase_to_end_of_line trims from the cursor. */
void check_erase()
{
    TerminalBuffer b(20, 5);
    b.write("hello");
    b.set_cursor(0, 2);
    b.erase_to_end_of_line();
    expect_text(b.line(0), U"he", "erase: the tail is gone");
}

/* Resizing changes the grid's shape. */
void check_resize()
{
    TerminalBuffer b(10, 3);
    b.resize(20, 5);
    expect_int(b.columns(), 20, "resize: the new width");
    expect_int(b.rows(), 5, "resize: the new height");
}

/* Lines that leave the top go to scrollback; the viewport walks it. */
void check_scrollback()
{
    TerminalBuffer b(10, 2);
    b.write("1\n2\n3\n4");
    expect_int(b.line_count(), 4, "scrollback: every line is kept");
    expect_int(b.scrollback_lines(), 2, "scrollback: two lines behind the viewport");
    expect_int(b.visible_first_line(), 2, "scrollback: the viewport is at the end");
    b.scroll_by(1);
    expect_int(b.visible_first_line(), 1, "scrollback: one line up");
    b.scroll_by(10);
    expect_int(b.visible_first_line(), 0, "scrollback: clamped at the top");
    b.scroll_to_bottom();
    expect_int(b.visible_first_line(), 2, "scrollback: back to the end");
}

/* A budget drops the oldest lines and no newer ones. */
void check_budget()
{
    TerminalBuffer b(10, 2);
    b.set_scrollback_budget(1);
    b.write("1\n2\n3\n4\n5");
    expect_int(b.line_count(), 1, "budget: all but the newest line were dropped");
    expect_text(b.line(0), U"5", "budget: the newest line is the survivor");
}

/* BiDi: an LTR line with a trailing RTL run reorders for display. */
void check_bidi()
{
    TerminalBuffer b(20, 5);
    b.write("abc \u05D0\u05D1\u05D2"); /* abc ALEF BET GIMEL */
    expect_text(b.line(0), U"abc \u05D0\u05D1\u05D2", "bidi: the logical line is unchanged");
    expect_text(b.visual_line(0), U"abc \u05D2\u05D1\u05D0",
                "bidi: the RTL run is reversed for display");
    /* The logical cell 4 (alef) is drawn at visual position 6; cell 6 (gimel)
     * at visual 4. */
    expect_int(b.visual_column(0, 4), 6, "bidi: the cursor maps alef to its display column");
    expect_int(b.visual_column(0, 6), 4, "bidi: the cursor maps gimel to its display column");
}

} // namespace

int main()
{
    check_append();
    check_newline();
    check_carriage_return();
    check_backspace();
    check_wide();
    check_combining();
    check_tab();
    check_wrap();
    check_erase();
    check_resize();
    check_scrollback();
    check_budget();
    check_bidi();

    std::printf("terminal: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
