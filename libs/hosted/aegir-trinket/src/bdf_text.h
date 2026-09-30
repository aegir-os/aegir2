/*
 * The BDF text a face's header is read with (specs/fonts.md, specs/trinket/overview.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A BDF line is a view into the font buffer: it is not NUL-terminated at its
 * end, so `sscanf` cannot read it. These are the two shapes both readers need
 * -- the catalog's probe, which reads a header, and `BitmapFont::load_bdf`,
 * which reads the whole face -- kept in one place rather than twice.
 */

#ifndef AEGIR_TRINKET_BDF_TEXT_H
#define AEGIR_TRINKET_BDF_TEXT_H

#include <string>
#include <string_view>

namespace aegir::trinket::detail {

inline bool starts_with(std::string_view line, std::string_view prefix) {
    return line.size() >= prefix.size() &&
           line.compare(0, prefix.size(), prefix) == 0;
}

/* A decimal integer off the front of `text`, the way a BDF line carries its
 * fields. */
inline bool next_int(std::string_view& text, int& value) {
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    text.remove_prefix(i);
    if (text.empty()) return false;
    bool negative = false;
    size_t j = 0;
    if (text[j] == '-') {
        negative = true;
        ++j;
    }
    if (j >= text.size() || text[j] < '0' || text[j] > '9') return false;
    int parsed = 0;
    while (j < text.size() && text[j] >= '0' && text[j] <= '9') {
        parsed = parsed * 10 + (text[j] - '0');
        ++j;
    }
    text.remove_prefix(j);
    value = negative ? -parsed : parsed;
    return true;
}

/* A BDF property's text: the leading spaces, and the quotes a string property
 * carries, are not part of the value (FAMILY_NAME "Terminus"). */
inline std::string property_text(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    return std::string(text);
}

/* One line off `header`, CR stripped, advancing `cursor`. */
inline bool next_line(std::string_view header, size_t& cursor, std::string_view& line) {
    if (cursor >= header.size()) return false;
    size_t const newline = header.find('\n', cursor);
    line = header.substr(cursor, newline == std::string_view::npos ? std::string_view::npos
                                                                   : newline - cursor);
    cursor = newline == std::string_view::npos ? header.size() : newline + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return true;
}

}  // namespace aegir::trinket::detail

#endif  // AEGIR_TRINKET_BDF_TEXT_H
