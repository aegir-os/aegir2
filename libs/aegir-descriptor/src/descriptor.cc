/*
 * Reading key=value row files -- implementation. See include/aegir/descriptor.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/descriptor.h>

namespace aegir::descriptor {

namespace {

bool space(char c) noexcept
{
    return c == ' ' || c == '\t';
}

}  // namespace

bool key_is(Field const &field, char const *key) noexcept
{
    uint32_t i = 0;
    while (i < field.key_length && key[i] != '\0' && field.key[i] == key[i]) {
        ++i;
    }
    return i == field.key_length && key[i] == '\0';
}

bool value_is(Field const &field, char const *value) noexcept
{
    uint32_t i = 0;
    while (i < field.value_length && value[i] != '\0' && field.value[i] == value[i]) {
        ++i;
    }
    return i == field.value_length && value[i] == '\0';
}

uint64_t number(Field const &field, bool *ok) noexcept
{
    uint64_t value = 0;
    if (field.value_length == 0) {
        *ok = false;
        return 0;
    }
    for (uint32_t i = 0; i < field.value_length; ++i) {
        char const c = field.value[i];
        if (c < '0' || c > '9') {
            *ok = false;
            return 0;
        }
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    *ok = true;
    return value;
}

Reader::Reader(void const *text, uint64_t bytes) noexcept
    : text_(static_cast<char const *>(text)), bytes_(bytes), at_(0), row_end_(0),
      field_at_(0), in_row_(false)
{
}

bool Reader::next_row() noexcept
{
    while (at_ < bytes_) {
        uint64_t const line = at_;
        uint64_t end = at_;
        while (end < bytes_ && text_[end] != '\n') {
            ++end;
        }
        at_ = end < bytes_ ? end + 1 : bytes_;
        /* The first non-space character decides: nothing (blank), a comment, or
         * a row. */
        uint64_t first = line;
        while (first < end && space(text_[first])) {
            ++first;
        }
        if (first == end || text_[first] == '#') {
            continue;
        }
        row_end_ = end;
        field_at_ = first;
        in_row_ = true;
        return true;
    }
    in_row_ = false;
    return false;
}

bool Reader::next_field(Field &field) noexcept
{
    if (!in_row_) {
        return false;
    }
    while (field_at_ < row_end_ && space(text_[field_at_])) {
        ++field_at_;
    }
    if (field_at_ >= row_end_) {
        return false;
    }
    uint64_t const token = field_at_;
    while (field_at_ < row_end_ && !space(text_[field_at_])) {
        ++field_at_;
    }
    /* A token without an '=' is not a field; it is a malformed row, and the
     * honest answer is a field with an empty value -- the consumer's required-
     * field check is what rejects it, with the row still readable. */
    uint64_t equals = token;
    while (equals < field_at_ && text_[equals] != '=') {
        ++equals;
    }
    field.key = text_ + token;
    field.key_length = static_cast<uint32_t>(equals - token);
    if (equals < field_at_) {
        field.value = text_ + equals + 1;
        field.value_length = static_cast<uint32_t>(field_at_ - equals - 1);
    } else {
        field.value = text_ + field_at_;
        field.value_length = 0;
    }
    return true;
}

}  // namespace aegir::descriptor
