/*
 * LineEditor implementation (specs/terminal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/line_editor.h>

#include <aegir/trinket/unicode.h>

#include <algorithm>

namespace aegir::trinket {

LineEditor::LineEditor(TerminalBuffer& buffer) : buffer_(buffer) {}

void LineEditor::set_prompt(std::u32string_view prompt)
{
    prompt_ = std::u32string(prompt);
}

void LineEditor::set_prompt(std::string_view prompt_utf8)
{
    prompt_ = utf8_to_utf32(prompt_utf8);
}

void LineEditor::begin()
{
    if (buffer_.line_columns(buffer_.cursor_line()) > 0) {
        buffer_.newline();
    }
    start_line_ = buffer_.cursor_line();
    line_.clear();
    cursor_ = 0;
    rendered_rows_ = 1;
    editing_ = true;
    history_index_ = -1;
    draft_.clear();
    buffer_.write(prompt_);
    buffer_.scroll_to_bottom();
}

void LineEditor::redraw()
{
    int const width = std::max(1, buffer_.columns());

    /* Erase what the last render left, wrapped rows included. */
    for (int row = 0; row < rendered_rows_; ++row) {
        buffer_.set_cursor(start_line_ + row, 0);
        buffer_.erase_to_end_of_line();
    }

    buffer_.set_cursor(start_line_, 0);
    buffer_.write(prompt_);
    buffer_.write(line_);

    /* The cursor's cell index: one per code point, plus the prompt's. A row
     * holds `width` cells; a cursor exactly at a row's end sits on that row's
     * last cell rather than the next row's first. */
    int const total = static_cast<int>(prompt_.size() + line_.size());
    int const before = static_cast<int>(prompt_.size()) + cursor_;
    int line_index = start_line_;
    int cell_index = before;
    if (before > 0 && before % width == 0) {
        line_index = start_line_ + before / width - 1;
        cell_index = width;
    } else {
        line_index = start_line_ + before / width;
        cell_index = before % width;
    }
    buffer_.set_cursor(line_index, cell_index);
    rendered_rows_ = total == 0 ? 1 : (total - 1) / width + 1;
    buffer_.scroll_to_bottom();
}

void LineEditor::insert(char32_t cp)
{
    if (cp < 32) {
        return;
    }
    line_.insert(line_.begin() + cursor_, cp);
    ++cursor_;
}

void LineEditor::clear_line()
{
    line_.clear();
    cursor_ = 0;
}

void LineEditor::history_move(int direction)
{
    if (history_.empty()) {
        return;
    }
    if (direction < 0) {
        if (history_index_ == -1) {
            draft_ = line_;
            history_index_ = static_cast<int>(history_.size()) - 1;
        } else if (history_index_ > 0) {
            --history_index_;
        } else {
            return;
        }
        line_ = history_[static_cast<std::size_t>(history_index_)];
    } else {
        if (history_index_ == -1) {
            return;
        }
        if (history_index_ + 1 < static_cast<int>(history_.size())) {
            ++history_index_;
            line_ = history_[static_cast<std::size_t>(history_index_)];
        } else {
            history_index_ = -1;
            line_ = draft_;
        }
    }
    cursor_ = static_cast<int>(line_.size());
    redraw();
}

bool LineEditor::on_key(KeyEvent const& event)
{
    if (!editing_ || !event.pressed) {
        return false;
    }
    switch (event.code) {
    case KeyCode::ENTER: {
        std::u32string const submitted = line_;
        if (!submitted.empty() &&
            (history_.empty() || history_.back() != submitted)) {
            history_.push_back(submitted);
        }
        buffer_.newline();
        editing_ = false;
        if (on_line_) {
            on_line_(submitted);
        } else {
            begin();
        }
        return true;
    }
    case KeyCode::BACKSPACE:
        if ((event.modifiers & 1u) != 0) {
            clear_line(); /* Shift-Backspace clears the line. */
        } else if (cursor_ > 0) {
            line_.erase(line_.begin() + (cursor_ - 1));
            --cursor_;
        }
        redraw();
        return true;
    case KeyCode::DELETE_KEY:
        if (cursor_ < static_cast<int>(line_.size())) {
            line_.erase(line_.begin() + cursor_);
        }
        redraw();
        return true;
    case KeyCode::LEFT:
        if (cursor_ > 0) {
            --cursor_;
        }
        redraw();
        return true;
    case KeyCode::RIGHT:
        if (cursor_ < static_cast<int>(line_.size())) {
            ++cursor_;
        }
        redraw();
        return true;
    case KeyCode::HOME:
        cursor_ = 0;
        redraw();
        return true;
    case KeyCode::END:
        cursor_ = static_cast<int>(line_.size());
        redraw();
        return true;
    case KeyCode::UP:
        history_move(-1);
        return true;
    case KeyCode::DOWN:
        history_move(1);
        return true;
    default:
        if (event.text >= 32) {
            insert(event.text);
            redraw();
            return true;
        }
        return false;
    }
}

} // namespace aegir::trinket
