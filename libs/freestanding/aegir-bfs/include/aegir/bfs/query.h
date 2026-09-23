/*
 * The Be File System's query language: an expression over attributes.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Haiku's query language (specs/bfs.md): equations of the form
 * `attribute op value`, combined with `&` (and) and `|` (or). The operators
 * are `=`, `==`, `!=`, `<`, `<=`, `>`, `>=`. `name`, `size` and
 * `last_modified` are the standard attributes every inode has; any other name
 * is a real attribute, read through the volume's metadata layer.
 *
 * A query is parsed once into a postfix term list and then evaluated against
 * each candidate inode. The parser is small and allocates nothing, so the
 * service can hold a parsed query across calls.
 */

#ifndef AEGIR_BFS_QUERY_H
#define AEGIR_BFS_QUERY_H

#include <aegir/bfs/layout.h>
#include <aegir/bfs/volume.h>
#include <stdint.h>

namespace aegir::bfs {

/** A query's literal, and the operator joining an equation. The attribute's
 *  own type decides how the literal compares. */
enum QueryOp : uint8_t {
    kQueryEqual,
    kQueryUnequal,
    kQueryGreater,
    kQueryLess,
    kQueryGreaterEqual,
    kQueryLessEqual,
    kQueryAnd,
    kQueryOr,
};

enum class QueryLiteral : uint8_t { String, Integer, Unsigned, Real, Boolean };

/** The most bytes a literal may hold. A longer one is a syntax error. */
constexpr uint32_t kMaxQueryValue = 64;

/** One `attribute op value`. */
struct QueryEquation {
    uint8_t op;
    QueryLiteral literal;
    char attribute[kMaxName];
    uint32_t attribute_length;
    char value[kMaxQueryValue];
    uint32_t value_length;
    int64_t integer;
    uint64_t unsigned_integer;
    double real;
    bool boolean;
};

/** A term is either an equation or a logical operator, in postfix order. */
struct QueryTerm {
    bool is_operator;
    uint8_t op;
    QueryEquation equation;
};

/** The most terms a query may have. A query is short by nature; a longer one
 *  is a syntax error rather than a silently dropped clause. */
constexpr uint32_t kMaxQueryTerms = 16;

class Query {
public:
    /** Parse `text` into postfix terms. False on any syntax error, leaving
     *  the query invalid. */
    bool parse(char const *text, uint32_t length) noexcept;

    bool valid() const noexcept { return count_ != 0; }

    /** The one equation, when the query is exactly one `attribute op value`;
     *  null when it is compound. A lone equality is what an index answers. */
    QueryEquation const *single_equation() const noexcept
    {
        return count_ == 1 && !terms_[0].is_operator ? &terms_[0].equation
                                                     : nullptr;
    }

    /** True when `inode` satisfies the query. The volume is needed for an
     *  equation on a real attribute. */
    bool matches(Volume const &volume, Inode const &inode) const noexcept;

    /* The parser's own append points; public only because the recursive
     * descent is a free function. */
    bool push_equation(QueryEquation const &equation) noexcept;
    bool push_operator(uint8_t op) noexcept;

private:
    bool match_equation(Volume const &volume, Inode const &inode,
                        QueryEquation const &equation) const noexcept;

    QueryTerm terms_[kMaxQueryTerms];
    uint32_t count_ = 0;
};

}  // namespace aegir::bfs

#endif  // AEGIR_BFS_QUERY_H