/*
 * The condition an `If` tests (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/script/condition.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aegir::script {

namespace {

char lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_folded(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

/* A whole decimal, optionally signed: the form a numeric comparison needs.
 * "12x" and "" are not numbers, so they compare as text. */
bool parse_int(std::string const &text, std::int64_t &out) noexcept
{
    if (text.empty()) {
        return false;
    }
    bool negative = false;
    std::size_t i = 0;
    if (text[0] == '-') {
        negative = true;
        i = 1;
    } else if (text[0] == '+') {
        i = 1;
    }
    if (i == text.size()) {
        return false;
    }
    std::int64_t value = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (text[i] - '0');
    }
    out = negative ? -value : value;
    return true;
}

bool op_from_word(std::string_view word, CompareOp &out) noexcept
{
    if (equals_folded(word, "EQ")) {
        out = CompareOp::Eq;
    } else if (equals_folded(word, "NE")) {
        out = CompareOp::Ne;
    } else if (equals_folded(word, "GT")) {
        out = CompareOp::Gt;
    } else if (equals_folded(word, "GE")) {
        out = CompareOp::Ge;
    } else if (equals_folded(word, "LT")) {
        out = CompareOp::Lt;
    } else if (equals_folded(word, "LE")) {
        out = CompareOp::Le;
    } else {
        return false;
    }
    return true;
}

std::string fold(std::string const &text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text) {
        out.push_back(lower(c));
    }
    return out;
}

}  // namespace

bool compare_strings(CompareOp op, std::string const &a, std::string const &b)
{
    std::int64_t left = 0;
    std::int64_t right = 0;
    if (parse_int(a, left) && parse_int(b, right)) {
        switch (op) {
        case CompareOp::Eq:
            return left == right;
        case CompareOp::Ne:
            return left != right;
        case CompareOp::Gt:
            return left > right;
        case CompareOp::Ge:
            return left >= right;
        case CompareOp::Lt:
            return left < right;
        case CompareOp::Le:
            return left <= right;
        }
    }
    int const order = fold(a).compare(fold(b));
    switch (op) {
    case CompareOp::Eq:
        return order == 0;
    case CompareOp::Ne:
        return order != 0;
    case CompareOp::Gt:
        return order > 0;
    case CompareOp::Ge:
        return order >= 0;
    case CompareOp::Lt:
        return order < 0;
    case CompareOp::Le:
        return order <= 0;
    }
    return false;
}

bool parse_condition(std::vector<std::string> const &words, Condition &out,
                     std::string &error)
{
    out = Condition{};
    std::size_t i = 0;
    while (i < words.size() && equals_folded(words[i], "NOT")) {
        out.negate = !out.negate;
        ++i;
    }
    if (i == words.size()) {
        error = "a condition, please";
        return false;
    }

    if (equals_folded(words[i], "EXISTS")) {
        if (i + 2 != words.size()) {
            error = "EXISTS takes one path";
            return false;
        }
        out.kind = Condition::Kind::Exists;
        out.path = words[i + 1];
        return true;
    }
    if (equals_folded(words[i], "WARN") || equals_folded(words[i], "ERROR") ||
        equals_folded(words[i], "FAIL")) {
        if (i + 1 != words.size()) {
            error = std::string(words[i]) + " takes nothing else";
            return false;
        }
        if (equals_folded(words[i], "WARN")) {
            out.kind = Condition::Kind::Warn;
        } else if (equals_folded(words[i], "ERROR")) {
            out.kind = Condition::Kind::Error;
        } else {
            out.kind = Condition::Kind::Fail;
        }
        return true;
    }

    CompareOp op = CompareOp::Eq;
    if (i + 3 == words.size() && op_from_word(words[i + 1], op)) {
        out.kind = Condition::Kind::Compare;
        out.lhs = words[i];
        out.op = op;
        out.rhs = words[i + 2];
        return true;
    }
    error = "unknown condition";
    return false;
}

}  // namespace aegir::script
