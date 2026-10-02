/*
 * Trinket TextDocument implementation (specs/trinket/editor.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/text_document.h>

#include <algorithm>

namespace aegir::trinket {

namespace {
std::u32string const kEmpty;

int clamp_int(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}
} // namespace

TextDocument::TextDocument(std::u32string_view text) {
    set_text(text);
}

void TextDocument::set_text(std::u32string_view text) {
    lines_.clear();
    std::u32string current;
    for (char32_t const cp : text) {
        if (cp == U'\n') {
            lines_.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(cp);
        }
    }
    lines_.push_back(std::move(current));
    cursor_ = Cursor{};
    want_column_ = 0;
    dirty_ = false;
    if (on_change) on_change();
}

std::u32string TextDocument::text() const {
    std::u32string out;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i != 0) out.push_back(U'\n');
        out += lines_[i];
    }
    return out;
}

std::u32string const& TextDocument::line(int row) const {
    if (row < 0 || row >= line_count()) return kEmpty;
    return lines_[static_cast<size_t>(row)];
}

int TextDocument::line_length(int row) const {
    return static_cast<int>(line(row).size());
}

void TextDocument::set_cursor(Cursor c) {
    cursor_.line = clamp_int(c.line, 0, line_count() - 1);
    cursor_.column = clamp_int(c.column, 0, line_length(cursor_.line));
    want_column_ = cursor_.column;
}

void TextDocument::insert(char32_t cp) {
    std::u32string& row = lines_[static_cast<size_t>(cursor_.line)];
    size_t const at = static_cast<size_t>(cursor_.column);
    if (mode_ == Mode::OVERWRITE && at < row.size()) {
        row[at] = cp;
    } else {
        row.insert(row.begin() + static_cast<long>(at), cp);
    }
    ++cursor_.column;
    want_column_ = cursor_.column;
    changed();
}

void TextDocument::newline() {
    std::u32string& row = lines_[static_cast<size_t>(cursor_.line)];
    size_t const at = static_cast<size_t>(cursor_.column);
    std::u32string tail = row.substr(at);
    row.resize(at);
    lines_.insert(lines_.begin() + cursor_.line + 1, std::move(tail));
    ++cursor_.line;
    cursor_.column = 0;
    want_column_ = 0;
    changed();
}

void TextDocument::backspace() {
    if (cursor_.column > 0) {
        std::u32string& row = lines_[static_cast<size_t>(cursor_.line)];
        row.erase(row.begin() + (cursor_.column - 1));
        --cursor_.column;
        want_column_ = cursor_.column;
        changed();
        return;
    }
    if (cursor_.line > 0) {
        /* The head of a line joins it to the one above: the caret lands where
         * the two met, which is that line's old length. */
        std::u32string const head = lines_[static_cast<size_t>(cursor_.line)];
        int const join = line_length(cursor_.line - 1);
        lines_[static_cast<size_t>(cursor_.line - 1)] += head;
        lines_.erase(lines_.begin() + cursor_.line);
        --cursor_.line;
        cursor_.column = join;
        want_column_ = join;
        changed();
    }
}

void TextDocument::erase() {
    std::u32string& row = lines_[static_cast<size_t>(cursor_.line)];
    if (cursor_.column < static_cast<int>(row.size())) {
        row.erase(row.begin() + cursor_.column);
        changed();
        return;
    }
    if (cursor_.line + 1 < line_count()) {
        row += lines_[static_cast<size_t>(cursor_.line + 1)];
        lines_.erase(lines_.begin() + cursor_.line + 1);
        changed();
    }
}

void TextDocument::move(int columns, int lines) {
    if (lines != 0) {
        int const target = clamp_int(cursor_.line + lines, 0, line_count() - 1);
        if (target != cursor_.line) {
            cursor_.line = target;
            /* Aim for the column the caret last held on its own, so a walk down
             * a short line and back returns it. */
            cursor_.column = std::min(want_column_, line_length(cursor_.line));
        }
    }
    if (columns != 0) {
        cursor_.column =
            clamp_int(cursor_.column + columns, 0, line_length(cursor_.line));
        want_column_ = cursor_.column;
    }
}

void TextDocument::home() {
    cursor_.column = 0;
    want_column_ = 0;
}

void TextDocument::end() {
    cursor_.column = line_length(cursor_.line);
    want_column_ = cursor_.column;
}

void TextDocument::clamp_cursor() {
    cursor_.line = clamp_int(cursor_.line, 0, line_count() - 1);
    cursor_.column = clamp_int(cursor_.column, 0, line_length(cursor_.line));
}

void TextDocument::changed() {
    clamp_cursor();
    dirty_ = true;
    if (on_change) on_change();
}

} // namespace aegir::trinket
