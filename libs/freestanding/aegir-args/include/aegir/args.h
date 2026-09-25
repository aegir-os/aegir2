/*
 * aegir::args: the Amiga's ReadArgs, as a library (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A command declares a template -- "FROM/A,TO,ALL/S" -- and gets its line
 * parsed into named values. The Amiga's conventions are the ones kept: a
 * template item is a name and a run of flags after '/', /A an argument that
 * must be present, /S a switch (present or absent), /K a keyword whose value
 * follows, /M one that may repeat, /N a decimal number. Items are separated by
 * spaces or commas, a name is case-insensitive, and NAME=VALUE names a value
 * exactly.
 *
 * Nothing here allocates and nothing here includes libc++ or seL4: the values
 * are pointers into the caller's own argv, so a command can be freestanding or
 * hosted and the parse costs no memory. That is also why a lookup re-walks the
 * line rather than storing a table -- there is no table to store.
 */

#ifndef AEGIR_ARGS_H
#define AEGIR_ARGS_H

#include <stdint.h>

namespace aegir::args {

/* One line, read against a template. The strings are the caller's: the
 * template text and the argv it was read against. */
struct Result {
    char const *tmpl;       /* the template, for usage() */
    int argc;
    char const *const *argv;
    char const *problem;    /* "" when the line is well-formed */
    char const *missing;    /* the first required name absent, or nullptr */
    uint32_t missing_length;

    bool ok() const noexcept { return problem[0] == '\0'; }
    char const *usage() const noexcept { return tmpl; }

    /* The value `name` was given, or nullptr when it was not supplied. A
     * present switch answers "" -- use present() to tell it from absent. For a
     * /M item this is its first occurrence; use count()/at() for the rest. */
    char const *value(char const *name) const noexcept;

    /* True when the item was supplied -- a switch seen, a value given, or a
     * keyword named. */
    bool present(char const *name) const noexcept;

    /* How many times a /M item was supplied, and the index-th of them. */
    uint32_t count(char const *name) const noexcept;
    char const *at(char const *name, uint32_t index) const noexcept;
};

/* Read `argv` (argc entries after the command's own name) against `tmpl`. A
 * malformed line reports through problem()/missing() and the caller prints
 * them and returns 10 (specs/dos.md). */
Result read(char const *tmpl, int argc, char const *const *argv) noexcept;

}  // namespace aegir::args

#endif  // AEGIR_ARGS_H
