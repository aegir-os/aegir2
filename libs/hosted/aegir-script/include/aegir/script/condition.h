/*
 * The condition an `If` tests (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * `If` is block-structured and its condition is the shell's to evaluate: this
 * file only parses the condition's words into a value and compares two
 * operands, so the syntax is host-tested and the shell supplies the two things
 * that are not pure -- whether a path exists and the last return code.
 *
 * Hosted C++ (std::string), no seL4 and no allocation policy of its own; the
 * pure parts are asserted by scripts/check_script.py.
 */

#ifndef AEGIR_SCRIPT_CONDITION_H
#define AEGIR_SCRIPT_CONDITION_H

#include <string>
#include <vector>

namespace aegir::script {

/** The comparison an If's operands take: the Amiga's EQ, NE, GT, GE, LT, LE.
 *  A comparison is numeric when both operands are decimal (a leading `-` or
 *  `+` is a sign), and case-blind and lexicographic otherwise. */
enum class CompareOp { Eq, Ne, Gt, Ge, Lt, Le };

/** An `If` condition. `negate` is a leading `NOT`; `kind` is which test; a
 *  path for Exists, a return-code threshold for Warn/Error/Fail, or two
 *  operands and an operator for Compare. */
struct Condition {
    enum class Kind { Exists, Warn, Error, Fail, Compare };
    bool negate = false;
    Kind kind = Kind::Exists;
    std::string path;
    CompareOp op = CompareOp::Eq;
    std::string lhs;
    std::string rhs;
};

/** Parse the words after `If` -- already substituted -- into a condition:
 *  `EXISTS <path>`, the return-code words `WARN`, `ERROR` or `FAIL`, or
 *  `<a> EQ|NE|GT|GE|LT|LE <b>`, each optionally behind `NOT`. False and fills
 *  `error` on anything else, including a misplaced argument. */
bool parse_condition(std::vector<std::string> const &words, Condition &out,
                     std::string &error);

/** Compare two operands under an Amiga operator. */
bool compare_strings(CompareOp op, std::string const &a, std::string const &b);

}  // namespace aegir::script

#endif  // AEGIR_SCRIPT_CONDITION_H
