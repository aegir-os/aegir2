/*
 * Reading key=value row files -- the driver registry and the partition-type
 * table (specs/services.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The format, in full: a row is a line; a field is `key=value`; fields are
 * separated by spaces; a line whose first non-space character is `#` is a
 * comment; blank lines are skipped. There is no quoting and no escaping, on
 * purpose: the day a value needs a space, the format has a problem a parser
 * option would only hide.
 *
 * The text is never copied and never owned -- fields point into it, so it must
 * outlive what is parsed from it. The initrd copy a spawning service is given
 * is mapped for its lifetime, which is why rows can be views rather than
 * allocations.
 */

#pragma once

#include <stdint.h>

namespace aegir::descriptor {

/** One `key=value` field: views into the row's text. Never NUL-terminated. */
struct Field {
    char const *key;
    uint32_t key_length;
    char const *value;
    uint32_t value_length;
};

/** True when the field's key is exactly this NUL-terminated literal. */
bool key_is(Field const &field, char const *key) noexcept;

/** The field's value as a number, or false through `ok` when it is not one. */
uint64_t number(Field const &field, bool *ok) noexcept;

/** True when the field's value is exactly this NUL-terminated literal. */
bool value_is(Field const &field, char const *value) noexcept;

/** Walks the rows of one file, and the fields of the current row. */
class Reader {
public:
    Reader(void const *text, uint64_t bytes) noexcept;

    /** Advance to the next row, skipping blank lines and comments. False at
     *  the end of the text. */
    bool next_row() noexcept;

    /** Advance to the next field of the current row. False at the end of the
     *  line. Calling it before the first `next_row` is an empty file's
     *  answer: false. */
    bool next_field(Field &field) noexcept;

private:
    char const *text_;
    uint64_t bytes_;
    uint64_t at_;       /* where the next row search starts */
    uint64_t row_end_;  /* the current row's newline (or the text's end) */
    uint64_t field_at_; /* where the next field search starts, within the row */
    bool in_row_;
};

}  // namespace aegir::descriptor
