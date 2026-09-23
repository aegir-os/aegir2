/*
 * The Be File System's query language -- implementation. See query.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The parser is a recursive descent over `or` and `and`, with an equation at
 * the leaves, emitting postfix terms. `and` binds tighter than `or`, and both
 * are left-associative. Nothing allocates: a query that does not fit the term
 * list is refused.
 */

#include <aegir/bfs/query.h>

#include <aegir/bfs/inode.h>
#include <aegir/metadata.h>

namespace aegir::bfs {

namespace {

bool is_space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void skip_space(char const *text, uint32_t length, uint32_t *pos) noexcept
{
    while (*pos < length && is_space(text[*pos])) {
        ++*pos;
    }
}

bool is_operator_start(char c) noexcept
{
    return c == '=' || c == '!' || c == '<' || c == '>' || c == '&' || c == '|' ||
           c == ')';
}

/* Read a quoted string starting at `*pos` (which is on the quote). */
bool read_quoted(char const *text, uint32_t length, uint32_t *pos, char *out,
                 uint32_t capacity, uint32_t *out_length) noexcept
{
    char const quote = text[*pos];
    ++*pos;
    uint32_t used = 0;
    while (*pos < length && text[*pos] != quote) {
        char c = text[*pos];
        if (c == '\\' && *pos + 1 < length) {
            ++*pos;
            c = text[*pos];
        }
        if (used >= capacity) {
            return false;
        }
        out[used++] = c;
        ++*pos;
    }
    if (*pos >= length) {
        return false; /* unterminated */
    }
    ++*pos; /* the closing quote */
    *out_length = used;
    return true;
}

/* A bare token: everything up to an operator, a close paren or the end, with
 * trailing whitespace trimmed. */
bool read_bare(char const *text, uint32_t length, uint32_t *pos, char *out,
               uint32_t capacity, uint32_t *out_length) noexcept
{
    uint32_t used = 0;
    while (*pos < length && !is_operator_start(text[*pos])) {
        if (used >= capacity) {
            return false;
        }
        out[used++] = text[*pos];
        ++*pos;
    }
    while (used != 0 && is_space(out[used - 1])) {
        --used;
    }
    *out_length = used;
    return true;
}

bool parse_integer(char const *text, uint32_t length, int64_t *out) noexcept
{
    if (length == 0) {
        return false;
    }
    uint32_t i = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        i = 1;
    }
    if (i >= length) {
        return false;
    }
    uint64_t value = 0;
    for (; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        uint64_t const digit = static_cast<uint64_t>(text[i] - '0');
        if (value > (static_cast<uint64_t>(INT64_MAX) - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    *out = negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
    return true;
}

bool parse_real(char const *text, uint32_t length, double *out) noexcept
{
    if (length == 0) {
        return false;
    }
    uint32_t i = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        i = 1;
    }
    double value = 0;
    bool digits = false;
    bool dot = false;
    for (; i < length; ++i) {
        char const c = text[i];
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            digits = true;
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    if (!digits || !dot) {
        return false;
    }
    *out = negative ? -value : value;
    return true;
}

/* The attribute's type_code as a family, so a literal knows how to compare. */
enum class Family { None, String, Integer, Unsigned, Real, Boolean };

Family family_of(uint32_t type) noexcept
{
    switch (type) {
    case metadata::kTypeString:
    case metadata::kTypeMime:
    case metadata::kTypeRaw:
        return Family::String;
    case metadata::kTypeInt32:
    case metadata::kTypeInt64:
    case metadata::kTypeInt16:
    case metadata::kTypeInt8:
        return Family::Integer;
    case metadata::kTypeUInt32:
    case metadata::kTypeUInt64:
    case metadata::kTypeUInt16:
    case metadata::kTypeUInt8:
        return Family::Unsigned;
    case metadata::kTypeFloat:
    case metadata::kTypeDouble:
        return Family::Real;
    case metadata::kTypeBool:
        return Family::Boolean;
    default:
        return Family::None;
    }
}

bool bytes_equal(char const *a, uint32_t a_length, char const *b,
                 uint32_t b_length) noexcept
{
    if (a_length != b_length) {
        return false;
    }
    for (uint32_t i = 0; i < a_length; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

int compare_bytes(char const *a, uint32_t a_length, char const *b,
                  uint32_t b_length) noexcept
{
    uint32_t const shared = a_length < b_length ? a_length : b_length;
    for (uint32_t i = 0; i < shared; ++i) {
        if (a[i] != b[i]) {
            return static_cast<uint8_t>(a[i]) < static_cast<uint8_t>(b[i]) ? -1 : 1;
        }
    }
    if (a_length == b_length) {
        return 0;
    }
    return a_length < b_length ? -1 : 1;
}

/* Decode an integer attribute of `type` into a signed or unsigned 64-bit. */
bool decode_integer(uint8_t const *data, uint32_t length, uint32_t type,
                    int64_t *signed_out, uint64_t *unsigned_out) noexcept
{
    switch (type) {
    case metadata::kTypeInt8:
        if (length != 1) return false;
        *signed_out = static_cast<int8_t>(data[0]);
        return true;
    case metadata::kTypeUInt8:
        if (length != 1) return false;
        *unsigned_out = data[0];
        return true;
    case metadata::kTypeInt16:
        if (length != 2) return false;
        *signed_out = static_cast<int16_t>(le16(data));
        return true;
    case metadata::kTypeUInt16:
        if (length != 2) return false;
        *unsigned_out = le16(data);
        return true;
    case metadata::kTypeInt32:
        if (length != 4) return false;
        *signed_out = static_cast<int32_t>(le32(data));
        return true;
    case metadata::kTypeUInt32:
        if (length != 4) return false;
        *unsigned_out = le32(data);
        return true;
    case metadata::kTypeInt64:
        if (length != 8) return false;
        *signed_out = le64_signed(data);
        return true;
    case metadata::kTypeUInt64:
        if (length != 8) return false;
        *unsigned_out = le64(data);
        return true;
    default:
        return false;
    }
}

bool decode_real(uint8_t const *data, uint32_t length, uint32_t type,
                 double *out) noexcept
{
    if (type == metadata::kTypeFloat && length == 4) {
        uint32_t const bits = le32(data);
        float value = 0;
        __builtin_memcpy(&value, &bits, sizeof(value));
        *out = value;
        return true;
    }
    if (type == metadata::kTypeDouble && length == 8) {
        uint64_t const bits = le64(data);
        double value = 0;
        __builtin_memcpy(&value, &bits, sizeof(value));
        *out = value;
        return true;
    }
    return false;
}

}  // namespace

bool Query::push_equation(QueryEquation const &equation) noexcept
{
    if (count_ >= kMaxQueryTerms) {
        return false;
    }
    terms_[count_].is_operator = false;
    terms_[count_].op = equation.op;
    terms_[count_].equation = equation;
    ++count_;
    return true;
}

bool Query::push_operator(uint8_t op) noexcept
{
    if (count_ >= kMaxQueryTerms) {
        return false;
    }
    terms_[count_].is_operator = true;
    terms_[count_].op = op;
    ++count_;
    return true;
}

namespace {

/* The parser state, so the recursive functions stay small. */
struct Parser {
    char const *text;
    uint32_t length;
    uint32_t pos;
    Query *query;

    bool equation() noexcept;
    bool conjunction() noexcept;
    bool disjunction() noexcept;
};

bool Parser::equation() noexcept
{
    skip_space(text, length, &pos);
    if (pos >= length) {
        return false;
    }
    QueryEquation out{};
    out.op = kQueryEqual;
    out.literal = QueryLiteral::String;
    out.value_length = 0;
    out.integer = 0;
    out.unsigned_integer = 0;
    out.real = 0;
    out.boolean = false;

    /* The attribute: quoted or a bare run. */
    if (text[pos] == '"' || text[pos] == '\'') {
        if (!read_quoted(text, length, &pos, out.attribute, sizeof(out.attribute),
                         &out.attribute_length)) {
            return false;
        }
    } else if (!read_bare(text, length, &pos, out.attribute,
                          sizeof(out.attribute), &out.attribute_length)) {
        return false;
    }
    if (out.attribute_length == 0) {
        return false;
    }

    skip_space(text, length, &pos);
    if (pos >= length) {
        return false;
    }
    switch (text[pos]) {
    case '=':
        out.op = kQueryEqual;
        ++pos;
        if (pos < length && text[pos] == '=') {
            ++pos;
        }
        break;
    case '!':
        if (pos + 1 >= length || text[pos + 1] != '=') {
            return false;
        }
        out.op = kQueryUnequal;
        pos += 2;
        break;
    case '<':
        out.op = kQueryLess;
        ++pos;
        if (pos < length && text[pos] == '=') {
            out.op = kQueryLessEqual;
            ++pos;
        }
        break;
    case '>':
        out.op = kQueryGreater;
        ++pos;
        if (pos < length && text[pos] == '=') {
            out.op = kQueryGreaterEqual;
            ++pos;
        }
        break;
    default:
        return false;
    }

    skip_space(text, length, &pos);
    if (pos >= length) {
        return false;
    }
    bool const quoted = text[pos] == '"' || text[pos] == '\'';
    if (quoted) {
        if (!read_quoted(text, length, &pos, out.value, sizeof(out.value),
                         &out.value_length)) {
            return false;
        }
        out.literal = QueryLiteral::String;
    } else {
        if (!read_bare(text, length, &pos, out.value, sizeof(out.value),
                       &out.value_length)) {
            return false;
        }
        if (out.value_length == 0) {
            return false;
        }
        int64_t integer = 0;
        double real = 0;
        if (parse_integer(out.value, out.value_length, &integer)) {
            out.literal = QueryLiteral::Integer;
            out.integer = integer;
        } else if (parse_real(out.value, out.value_length, &real)) {
            out.literal = QueryLiteral::Real;
            out.real = real;
        } else if (bytes_equal(out.value, out.value_length, "true", 4)) {
            out.literal = QueryLiteral::Boolean;
            out.boolean = true;
        } else if (bytes_equal(out.value, out.value_length, "false", 5)) {
            out.literal = QueryLiteral::Boolean;
            out.boolean = false;
        } else {
            out.literal = QueryLiteral::String;
        }
    }
    return query->push_equation(out);
}

bool Parser::conjunction() noexcept
{
    if (!equation()) {
        return false;
    }
    for (;;) {
        skip_space(text, length, &pos);
        if (pos >= length || text[pos] != '&') {
            return true;
        }
        ++pos;
        if (pos < length && text[pos] == '&') {
            ++pos;
        }
        if (!equation() || !query->push_operator(kQueryAnd)) {
            return false;
        }
    }
}

bool Parser::disjunction() noexcept
{
    if (!conjunction()) {
        return false;
    }
    for (;;) {
        skip_space(text, length, &pos);
        if (pos >= length || text[pos] != '|') {
            return true;
        }
        ++pos;
        if (pos < length && text[pos] == '|') {
            ++pos;
        }
        if (!conjunction() || !query->push_operator(kQueryOr)) {
            return false;
        }
    }
}

}  // namespace

bool Query::parse(char const *text, uint32_t length) noexcept
{
    count_ = 0;
    if (text == nullptr || length == 0) {
        return false;
    }
    Parser parser{text, length, 0, this};
    if (!parser.disjunction()) {
        count_ = 0;
        return false;
    }
    skip_space(text, length, &parser.pos);
    if (parser.pos != length) {
        count_ = 0; /* trailing junk */
        return false;
    }
    return count_ != 0;
}

bool Query::match_equation(Volume const &volume, Inode const &inode,
                           QueryEquation const &equation) const noexcept
{
    uint8_t data[kMaxQueryValue];
    uint32_t length = 0;
    uint32_t type = 0;
    bool have_value = true;

    if (bytes_equal(equation.attribute, equation.attribute_length, "name", 4)) {
        uint32_t const copy =
            inode.name_length < sizeof(data) ? inode.name_length : sizeof(data);
        for (uint32_t i = 0; i < copy; ++i) {
            data[i] = static_cast<uint8_t>(inode.name[i]);
        }
        length = copy;
        type = metadata::kTypeString;
    } else if (bytes_equal(equation.attribute, equation.attribute_length, "size",
                           4)) {
        put_le64(data, static_cast<uint64_t>(inode.size));
        length = 8;
        type = metadata::kTypeInt64;
    } else if (bytes_equal(equation.attribute, equation.attribute_length,
                           "last_modified", 13)) {
        put_le64(data, static_cast<uint64_t>(inode.mtime >> 16));
        length = 8;
        type = metadata::kTypeInt64;
    } else {
        uint64_t attribute_size = 0;
        if (!volume.attr_stat(inode, equation.attribute, equation.attribute_length,
                              &type, &attribute_size)) {
            return false; /* no such attribute: no match */
        }
        uint32_t wanted = attribute_size < sizeof(data)
                              ? static_cast<uint32_t>(attribute_size)
                              : sizeof(data);
        length = wanted;
        if (!volume.attr_read(inode, equation.attribute,
                              equation.attribute_length, 0, data, &length)) {
            return false;
        }
        have_value = true;
    }
    if (!have_value) {
        return false;
    }

    Family const family = family_of(type);
    bool equal = false;
    int order = 0;
    switch (equation.literal) {
    case QueryLiteral::String: {
        if (family != Family::String) {
            return false;
        }
        equal = bytes_equal(reinterpret_cast<char const *>(data), length,
                            equation.value, equation.value_length);
        order = compare_bytes(reinterpret_cast<char const *>(data), length,
                              equation.value, equation.value_length);
        break;
    }
    case QueryLiteral::Boolean: {
        if (family != Family::Boolean || length != 1) {
            return false;
        }
        equal = (data[0] != 0) == equation.boolean;
        order = equal ? 0 : 1;
        break;
    }
    case QueryLiteral::Integer:
    case QueryLiteral::Unsigned: {
        if (family == Family::Real) {
            double value = 0;
            if (!decode_real(data, length, type, &value)) {
                return false;
            }
            double const literal = equation.literal == QueryLiteral::Integer
                                       ? static_cast<double>(equation.integer)
                                       : static_cast<double>(equation.unsigned_integer);
            equal = value == literal;
            order = value < literal ? -1 : (value > literal ? 1 : 0);
            break;
        }
        if (family != Family::Integer && family != Family::Unsigned) {
            return false;
        }
        int64_t signed_value = 0;
        uint64_t unsigned_value = 0;
        if (!decode_integer(data, length, type, &signed_value, &unsigned_value)) {
            return false;
        }
        bool const is_signed = family == Family::Integer;
        if (is_signed) {
            int64_t const literal = equation.literal == QueryLiteral::Integer
                                        ? equation.integer
                                        : static_cast<int64_t>(equation.unsigned_integer);
            equal = signed_value == literal;
            order = signed_value < literal ? -1 : (signed_value > literal ? 1 : 0);
        } else {
            uint64_t const literal = equation.literal == QueryLiteral::Unsigned
                                         ? equation.unsigned_integer
                                         : static_cast<uint64_t>(equation.integer);
            equal = unsigned_value == literal;
            order = unsigned_value < literal ? -1 : (unsigned_value > literal ? 1 : 0);
        }
        break;
    }
    case QueryLiteral::Real: {
        if (family != Family::Real && family != Family::Integer &&
            family != Family::Unsigned) {
            return false;
        }
        double value = 0;
        if (family == Family::Real) {
            if (!decode_real(data, length, type, &value)) {
                return false;
            }
        } else {
            int64_t signed_value = 0;
            uint64_t unsigned_value = 0;
            if (!decode_integer(data, length, type, &signed_value, &unsigned_value)) {
                return false;
            }
            value = family == Family::Integer ? static_cast<double>(signed_value)
                                              : static_cast<double>(unsigned_value);
        }
        equal = value == equation.real;
        order = value < equation.real ? -1 : (value > equation.real ? 1 : 0);
        break;
    }
    }

    switch (equation.op) {
    case kQueryEqual:
        return equal;
    case kQueryUnequal:
        return !equal;
    case kQueryGreater:
        return order > 0;
    case kQueryLess:
        return order < 0;
    case kQueryGreaterEqual:
        return order >= 0;
    case kQueryLessEqual:
        return order <= 0;
    default:
        return false;
    }
}

bool Query::matches(Volume const &volume, Inode const &inode) const noexcept
{
    if (count_ == 0) {
        return false;
    }
    bool stack[kMaxQueryTerms];
    uint32_t depth = 0;
    for (uint32_t i = 0; i < count_; ++i) {
        QueryTerm const &term = terms_[i];
        if (!term.is_operator) {
            if (depth >= kMaxQueryTerms) {
                return false;
            }
            stack[depth++] = match_equation(volume, inode, term.equation);
            continue;
        }
        if (depth < 2) {
            return false;
        }
        bool const right = stack[--depth];
        bool const left = stack[--depth];
        stack[depth++] = term.op == kQueryAnd ? (left && right) : (left || right);
    }
    return depth == 1 && stack[0];
}

}  // namespace aegir::bfs