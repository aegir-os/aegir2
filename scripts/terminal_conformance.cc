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

#include <aegir/console_stream.h>
#include <aegir/nmspace.h>
#include <aegir/trinket/line_editor.h>
#include <aegir/trinket/terminal_buffer.h>
#include <aegir/trinket/unicode.h>
#include <aegir/trinket/widget.h>

#include "console_stream_server.h"

#include <cstdio>
#include <string>
#include <cstdint>

namespace {

using aegir::trinket::KeyCode;
using aegir::trinket::KeyEvent;
using aegir::trinket::LineEditor;
using aegir::terminal::ConsoleStreamServer;
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

    /* L4: a mirror is applied only at an odd (RTL) resolved level. An LTR
     * line's '>' stays '>', and an RTL paragraph's becomes '<'. */
    TerminalBuffer ltr(20, 5);
    ltr.write("Home:>");
    expect_text(ltr.visual_line(0), U"Home:>", "bidi: an LTR '>' is not mirrored");
    TerminalBuffer rtl(20, 5);
    rtl.write("\u05D0>");
    expect_text(rtl.visual_line(0), U"<\u05D0", "bidi: an RTL '>' is mirrored");
}

KeyEvent char_key(char32_t text)
{
    KeyEvent event;
    event.text = text;
    event.pressed = true;
    return event;
}

KeyEvent code_key(KeyCode code, uint32_t modifiers = 0)
{
    KeyEvent event;
    event.code = code;
    event.modifiers = modifiers;
    event.pressed = true;
    return event;
}

/* A cooked line: keys type it, Enter ends it, and the server hands the client
 * the finished line and lets it begin the next prompt (specs/terminal.md). */
void check_line_editor()
{
    TerminalBuffer b(20, 5);
    ConsoleStreamServer server(b);
    expect_int(server.open_local(1, "Home>") ? 1 : 0, 1, "editor: the stream opens");
    expect_int(server.open_local(1, "again") ? 1 : 0, 0, "editor: a second open is refused");
    server.begin(1);

    LineEditor* const editor = server.editor(1);
    expect_int(editor == nullptr ? 0 : 1, 1, "editor: the cooked stream has a line editor");
    for (char32_t const cp : std::u32string(U"abc")) {
        expect_int(editor->on_key(char_key(cp)) ? 1 : 0, 1, "editor: a printable is consumed");
    }
    expect_text(editor->line(), U"abc", "editor: the line holds the typed keys");
    /* Enter ends the line and the server holds it, not the editor. */
    expect_int(editor->on_key(code_key(KeyCode::ENTER)) ? 1 : 0, 1, "editor: Enter is consumed");
    expect_int(server.line_ready(1) ? 1 : 0, 1, "editor: the finished line is ready");
    expect_text(server.take_line(1), U"abc", "editor: take_line returns it");
    expect_int(server.line_ready(1) ? 1 : 0, 0, "editor: it is ready only once");

    /* History: the taken line is recalled by the up arrow. */
    server.begin(1);
    editor->on_key(code_key(KeyCode::UP));
    expect_text(editor->line(), U"abc", "editor: the up arrow recalls the last line");

    /* Shift-Backspace clears the line without editing it. */
    editor->on_key(code_key(KeyCode::BACKSPACE, 1));
    expect_text(editor->line(), U"", "editor: Shift-Backspace clears the line");
}

/* The wire path, since a host test cannot call an endpoint: the same handler
 * the on_call in the terminal will reach. */
void check_stream_wire()
{
    TerminalBuffer b(20, 5);
    ConsoleStreamServer server(b);
    uint64_t reply[1] = {0};

    /* open: mode, then the prompt string. */
    uint64_t open_words[1 + aegir::nmspace::kPathMax / 8 + 1];
    open_words[0] = aegir::console::kStreamModeCooked;
    uint32_t const open_count =
        1 + aegir::nmspace::pack_string(open_words + 1, "Home>", 5,
                                        aegir::nmspace::kPathMax);
    uint32_t answer = server.handle(aegir::console::kStreamMethodOpen, open_words,
                                    open_count, 7, reply, 1);
    expect_int(answer == 1 && reply[0] == 1 ? 1 : 0, 1, "wire: open answers ok");

    /* write: the bytes as a string; the answer is the count. */
    uint64_t write_words[aegir::nmspace::kPathMax / 8 + 1];
    uint32_t const write_count =
        aegir::nmspace::pack_string(write_words, "hi\n", 3, aegir::nmspace::kPathMax);
    answer = server.handle(aegir::console::kStreamMethodWrite, write_words, write_count,
                           7, reply, 1);
    expect_int(answer == 1 && reply[0] == 3 ? 1 : 0, 1, "wire: write answers the count");
    expect_text(b.line(0), U"hi", "wire: the bytes reached the grid");

    /* Type a line into the editor, then read it back through the wire. The
     * client's loop calls read_line first, which begins the editor; keys then
     * go to it, and the next read_line answers the line. */
    LineEditor* const editor = server.editor(7);
    uint64_t in[aegir::console::kStreamBytesMax / 8 + 2];
    answer = server.handle(aegir::console::kStreamMethodReadLine, nullptr, 0, 7, in,
                           aegir::console::kStreamBytesMax / 8 + 2);
    expect_int(answer, 0, "wire: read_line with no line is empty");
    for (char32_t const cp : std::u32string(U"ls")) {
        editor->on_key(char_key(cp));
    }
    editor->on_key(code_key(KeyCode::ENTER));
    answer = server.handle(aegir::console::kStreamMethodReadLine, nullptr, 0, 7, in,
                           aegir::console::kStreamBytesMax / 8 + 2);
    char const* text = nullptr;
    uint32_t length = 0;
    expect_int(answer > 0 && aegir::nmspace::unpack_string(in, answer,
                                                            aegir::console::kStreamBytesMax,
                                                            &text, &length)
                    ? 1
                    : 0,
               1, "wire: read_line answers a string");
    expect_text(aegir::trinket::utf8_to_utf32(std::string_view(text, length)), U"ls",
                "wire: read_line is the typed line");
    answer = server.handle(aegir::console::kStreamMethodReadLine, nullptr, 0, 7, in,
                           aegir::console::kStreamBytesMax / 8 + 2);
    expect_int(answer, 0, "wire: read_line is empty with no line ready");

    /* exit: a command reports its status and the stream stays open -- it is
     * the shell's, and the command only inherited a copy. */
    uint64_t exit_words[1] = {7};
    server.handle(aegir::console::kStreamMethodExit, exit_words, 1, 7, reply, 1);
    expect_int(server.command_finished(7) ? 1 : 0, 1, "wire: exit marks the command done");
    expect_int(static_cast<int>(server.exit_status(7)), 7, "wire: exit records the status");
    server.clear_command(7);
    expect_int(server.command_finished(7) ? 1 : 0, 0, "wire: clear_command resets it");
    expect_int(server.editor(7) == nullptr ? 0 : 1, 1, "wire: exit leaves the stream open");

    /* close: the stream is dropped. */
    uint64_t close_words[1] = {0};
    server.handle(aegir::console::kStreamMethodClose, close_words, 1, 7, reply, 1);
    expect_int(server.editor(7) == nullptr ? 0 : 1, 0, "wire: close drops the stream");
    answer = server.handle(aegir::console::kStreamMethodWrite, write_words, write_count,
                           7, reply, 1);
    expect_int(answer == 1 && reply[0] == 0 ? 1 : 0, 1,
               "wire: a write to a closed stream writes nothing");
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
    check_line_editor();
    check_stream_wire();

    std::printf("terminal: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
