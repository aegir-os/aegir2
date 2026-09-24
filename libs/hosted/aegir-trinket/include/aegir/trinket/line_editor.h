/*
 * LineEditor: the console's command line (specs/terminal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The cooked half of a console stream: a prompt, a line being typed, a cursor,
 * and the history the up and down arrows walk. It edits a TerminalBuffer --
 * drawing the prompt and the line, and echoing every change -- and hands the
 * finished line to its client through a callback. The `CON:` handler owns one
 * per stream; a client that wants raw keys does not use it.
 */

#ifndef AEGIR_TRINKET_LINE_EDITOR_H
#define AEGIR_TRINKET_LINE_EDITOR_H

#include <aegir/trinket/terminal_buffer.h>
#include <aegir/trinket/widget.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aegir::trinket {

class LineEditor {
public:
    explicit LineEditor(TerminalBuffer& buffer);

    void set_prompt(std::u32string_view prompt);
    void set_prompt(std::string_view prompt_utf8);
    std::u32string const& prompt() const { return prompt_; }

    /* The finished line, called when Enter is pressed. The callback runs the
     * line and then calls begin() to draw the next prompt; the editor does not
     * draw it itself, so a client's output lands between the two. */
    void set_on_line(std::function<void(std::u32string const&)> on_line) {
        on_line_ = std::move(on_line);
    }

    /* Draw the prompt on a fresh line and reset the editing state. */
    void begin();

    /* One key. True when the editor consumed it (a client that also scrolls
     * must not act on the keys the editor used). */
    bool on_key(KeyEvent const& event);

    std::u32string const& line() const { return line_; }
    int cursor() const { return cursor_; }
    bool editing() const { return editing_; }
    std::vector<std::u32string> const& history() const { return history_; }

private:
    void redraw();
    void insert(char32_t cp);
    void clear_line();
    void history_move(int direction);

    TerminalBuffer& buffer_;
    std::u32string prompt_;
    std::u32string line_;
    int cursor_ = 0;
    int start_line_ = 0;
    int rendered_rows_ = 1;
    bool editing_ = false;
    std::vector<std::u32string> history_;
    int history_index_ = -1;
    std::u32string draft_;
    std::function<void(std::u32string const&)> on_line_;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_LINE_EDITOR_H
