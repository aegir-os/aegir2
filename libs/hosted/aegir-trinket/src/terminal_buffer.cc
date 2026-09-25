/*
 * The terminal's text model (specs/terminal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/terminal_buffer.h>

#include <aegir/trinket/bidi.h>
#include <aegir/trinket/unicode.h>

#include <algorithm>

#include "width_tables.h"

namespace aegir::trinket {

TerminalBuffer::TerminalBuffer(int columns, int rows)
    : columns_(columns < 1 ? 1 : columns), rows_(rows < 1 ? 1 : rows)
{
    lines_.emplace_back();
}

void TerminalBuffer::resize(int columns, int rows)
{
    ++version_;
    columns_ = columns < 1 ? 1 : columns;
    rows_ = rows < 1 ? 1 : rows;
    if (cursor_cell_ > static_cast<int>(current_line().size())) {
        cursor_cell_ = static_cast<int>(current_line().size());
    }
    scroll_to_bottom();
}

TerminalLine& TerminalBuffer::current_line()
{
    ensure_line(cursor_line_);
    return lines_[static_cast<std::size_t>(cursor_line_)];
}

void TerminalBuffer::ensure_line(int index)
{
    ++version_;
    while (static_cast<int>(lines_.size()) <= index) {
        lines_.emplace_back();
    }
}

void TerminalBuffer::write(std::string_view utf8)
{
    for (Utf8Iterator it(utf8), end = Utf8Iterator::end_of(utf8); it != end; ++it) {
        apply(*it);
    }
    scroll_offset_ = 0;
}

void TerminalBuffer::write(std::u32string_view text)
{
    for (char32_t const cp : text) {
        apply(cp);
    }
    scroll_offset_ = 0;
}

void TerminalBuffer::apply(char32_t cp)
{
    switch (cp) {
    case U'\n':
        newline();
        break;
    case U'\r':
        carriage_return();
        break;
    case U'\b':
        backspace();
        break;
    case U'\t':
        tab();
        break;
    default:
        put(cp);
        break;
    }
}

void TerminalBuffer::newline()
{
    ++cursor_line_;
    ensure_line(cursor_line_);
    cursor_cell_ = 0;
    scroll_offset_ = 0;
    enforce_budget();
}

void TerminalBuffer::carriage_return()
{
    cursor_cell_ = 0;
}

void TerminalBuffer::backspace()
{
    /* A terminal's backspace moves the cursor left; it does not erase. The
     * next write overwrites, and the line a client wants cleared it clears
     * with erase_to_end_of_line. A wide character is stepped over whole and a
     * combining mark does not stop the step. */
    TerminalLine const& line = current_line();
    int i = cursor_cell_ > static_cast<int>(line.size())
                ? static_cast<int>(line.size())
                : cursor_cell_;
    while (i > 0 && line[static_cast<std::size_t>(i - 1)].width == 0) {
        --i;
    }
    if (i > 0) {
        --i;
    }
    cursor_cell_ = i;
}

void TerminalBuffer::tab()
{
    int const next = (cursor_column() / 8 + 1) * 8;
    while (cursor_column() < next) {
        put(U' ');
    }
}

void TerminalBuffer::erase_to_end_of_line()
{
    ++version_;
    TerminalLine& line = current_line();
    if (cursor_cell_ < static_cast<int>(line.size())) {
        line.erase(line.begin() + cursor_cell_, line.end());
    }
}

void TerminalBuffer::clear()
{
    ++version_;
    lines_.clear();
    lines_.emplace_back();
    cursor_line_ = 0;
    cursor_cell_ = 0;
    scroll_offset_ = 0;
}

void TerminalBuffer::set_cursor(int line, int cell)
{
    cursor_line_ = line < 0 ? 0 : line;
    ensure_line(cursor_line_);
    TerminalLine const& current = lines_[static_cast<std::size_t>(cursor_line_)];
    cursor_cell_ = std::clamp(cell, 0, static_cast<int>(current.size()));
}

int TerminalBuffer::cursor_column() const
{
    if (cursor_line_ < 0 || cursor_line_ >= static_cast<int>(lines_.size())) {
        return 0;
    }
    TerminalLine const& line = lines_[static_cast<std::size_t>(cursor_line_)];
    int column = 0;
    int const limit = std::min(cursor_cell_, static_cast<int>(line.size()));
    for (int i = 0; i < limit; ++i) {
        column += line[static_cast<std::size_t>(i)].width;
    }
    return column;
}

void TerminalBuffer::put(char32_t cp)
{
    ++version_;
    int const width = detail::char_width(cp);
    if (width == 0) {
        TerminalLine& line = current_line();
        int const at = std::clamp(cursor_cell_, 0, static_cast<int>(line.size()));
        line.insert(line.begin() + at, TerminalCell{cp, 0});
        cursor_cell_ = at;
        return;
    }
    if (cursor_column() > 0 && cursor_column() + width > columns_) {
        newline();
    }
    TerminalLine& line = current_line();
    TerminalCell const cell{cp, static_cast<uint8_t>(width)};
    if (cursor_cell_ < static_cast<int>(line.size())) {
        line[static_cast<std::size_t>(cursor_cell_)] = cell;
    } else {
        line.push_back(cell);
    }
    ++cursor_cell_;
}

std::u32string TerminalBuffer::line(int index) const
{
    std::u32string result;
    if (index < 0 || index >= static_cast<int>(lines_.size())) {
        return result;
    }
    TerminalLine const& line = lines_[static_cast<std::size_t>(index)];
    result.reserve(line.size());
    for (TerminalCell const& cell : line) {
        result.push_back(cell.cp);
    }
    return result;
}

int TerminalBuffer::line_columns(int index) const
{
    if (index < 0 || index >= static_cast<int>(lines_.size())) {
        return 0;
    }
    int column = 0;
    for (TerminalCell const& cell : lines_[static_cast<std::size_t>(index)]) {
        column += cell.width;
    }
    return column;
}

std::u32string TerminalBuffer::visual_line(int index) const
{
    std::u32string result;
    for (TerminalCell const& cell : visual_cells(index)) {
        result.push_back(cell.cp);
    }
    return result;
}

namespace {

/* Whether one character can change the display order or be mirrored under UAX
 * #9. A line with none of these classes keeps the logical order, so the
 * algorithm -- and the allocations it makes for every visible line on every
 * keystroke -- is skipped for it. The classes are the RTL ones and the
 * explicit embedding, override and isolate controls (R, AL, AN and
 * LRE/RLE/LRO/RLO/PDF/LRI/RLI/FSI/PDI). */
bool reorders(char32_t cp) noexcept
{
    switch (bidi_class(cp)) {
    case BidiClass::R:
    case BidiClass::AL:
    case BidiClass::AN:
    case BidiClass::LRE:
    case BidiClass::RLE:
    case BidiClass::LRO:
    case BidiClass::RLO:
    case BidiClass::PDF:
    case BidiClass::LRI:
    case BidiClass::RLI:
    case BidiClass::FSI:
    case BidiClass::PDI:
        return true;
    default:
        return false;
    }
}

}  // namespace

std::vector<TerminalCell> TerminalBuffer::visual_cells(int index) const
{
    std::vector<TerminalCell> result;
    visual_cells_into(index, result);
    return result;
}

void TerminalBuffer::visual_cells_into(int index, std::vector<TerminalCell>& out) const
{
    out.clear();
    if (index < 0 || index >= static_cast<int>(lines_.size())) {
        return;
    }
    TerminalLine const& logical = lines_[static_cast<std::size_t>(index)];
    bool ordered = false;
    for (TerminalCell const& cell : logical) {
        if (reorders(cell.cp)) {
            ordered = true;
            break;
        }
    }
    if (!ordered) {
        out.assign(logical.begin(), logical.end());
        return;
    }
    std::u32string const codepoints = line(index);
    BidiParagraph const paragraph = analyze_paragraph(codepoints);
    out.reserve(logical.size());
    for (int const logical_index : paragraph.order) {
        TerminalCell cell = logical[static_cast<std::size_t>(logical_index)];
        /* L4: a character is drawn mirrored only when its *resolved* direction
         * is RTL -- an odd embedding level -- and the character is one that has
         * a mirror. Mirroring every cell turned an LTR prompt's `>` into `<`. */
        if ((paragraph.levels[static_cast<std::size_t>(logical_index)] & 1u) != 0) {
            cell.cp = mirror_char(cell.cp);
        }
        out.push_back(cell);
    }
}

int TerminalBuffer::visual_column(int index, int cell) const
{
    std::u32string const logical = line(index);
    if (logical.empty()) {
        return 0;
    }
    int const logical_index = std::clamp(cell, 0, static_cast<int>(logical.size()));
    bool ordered = false;
    for (char32_t cp : logical) {
        if (reorders(cp)) {
            ordered = true;
            break;
        }
    }
    if (!ordered) {
        int column = 0;
        for (int i = 0; i < logical_index; ++i) {
            column += detail::char_width(logical[static_cast<std::size_t>(i)]);
        }
        return column;
    }
    BidiParagraph const paragraph = analyze_paragraph(logical);
    std::vector<int> const logical_to_visual = paragraph.logical_to_visual();
    int const visual_index =
        logical_index < static_cast<int>(logical_to_visual.size())
            ? logical_to_visual[static_cast<std::size_t>(logical_index)]
            : static_cast<int>(paragraph.order.size());
    int column = 0;
    for (int v = 0; v < visual_index; ++v) {
        column += detail::char_width(logical[static_cast<std::size_t>(paragraph.order[static_cast<std::size_t>(v)])]);
    }
    return column;
}

std::u32string TerminalBuffer::text() const
{
    std::u32string result;
    for (int i = 0; i < line_count(); ++i) {
        if (i != 0) {
            result.push_back(U'\n');
        }
        result += line(i);
    }
    return result;
}

int TerminalBuffer::scrollback_lines() const
{
    return std::max(0, line_count() - rows_);
}

void TerminalBuffer::scroll_by(int lines)
{
    scroll_offset_ = std::clamp(scroll_offset_ + lines, 0, scrollback_lines());
}

void TerminalBuffer::scroll_to_bottom()
{
    scroll_offset_ = 0;
}

int TerminalBuffer::visible_first_line() const
{
    int const first = line_count() - rows_ - scroll_offset_;
    return first < 0 ? 0 : first;
}

std::size_t TerminalBuffer::retained_bytes() const
{
    std::size_t total = lines_.size() * sizeof(TerminalLine);
    for (TerminalLine const& line : lines_) {
        total += line.size() * sizeof(TerminalCell);
    }
    return total;
}

void TerminalBuffer::enforce_budget()
{
    if (scrollback_budget_ == 0) {
        return;
    }
    while (lines_.size() > 1 && retained_bytes() > scrollback_budget_) {
        lines_.pop_front();
        if (cursor_line_ > 0) {
            --cursor_line_;
        }
        if (scroll_offset_ > 0) {
            --scroll_offset_;
        }
    }
}

} // namespace aegir::trinket
