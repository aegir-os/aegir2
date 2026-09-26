/*
 * Variable substitution for the shell (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A line is not run as it is typed: `$name` and `{name}` stand for a variable,
 * `$1..$n` for the arguments a script was run with, and `"` groups a word that
 * holds a space. The words are the shell's to make before it looks for a
 * command, an alias or a redirection -- so substitution comes before all of
 * them. The value a variable expands to is inserted literally, never rescanned.
 *
 * Hosted C++ (std::string), no seL4 and no allocation policy of its own; the
 * pure parts are asserted by scripts/check_script.py.
 */

#ifndef AEGIR_SCRIPT_SUBSTITUTE_H
#define AEGIR_SCRIPT_SUBSTITUTE_H

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegir::script {

/** A script's bound arguments: what the interpreter substitutes into a line.
 *  `name` is `$0`, `positional` are `$1..$n`, and `named` are the `.KEY`
 *  parameters `{name}` reads. */
struct Arguments {
    std::string name;
    std::vector<std::string> positional;
    std::vector<std::pair<std::string, std::string>> named;

    /** The value bound to a `.KEY` name, or nullptr. */
    std::string const *find_named(std::string_view name) const noexcept;
};

/** The environment a substitution reads: true and fills `value` when `name`
 *  is set. The shell passes `aegir::environment`; the host tests pass a map. */
using Lookup = std::function<bool(std::string const &, std::string &)>;

/** A `.KEY` parameter: its name, and whether the script requires it. */
struct KeySymbol {
    std::string name;
    bool required = false;
};

/** True when `line` -- leading blanks aside, case-insensitively -- is a `.KEY`
 *  directive. */
bool is_key_line(std::string_view line) noexcept;

/** Parse a `.KEY` directive's argument list (the text after `.KEY`): a
 *  comma-separated list of `name` or `name/A`. False and fills `error` on a
 *  malformed list or a flag other than `/A` (specs/shell.md). */
bool parse_key_list(std::string_view line, std::vector<KeySymbol> &out,
                    std::string &error);

/** Bind a `.KEY` declaration to the arguments a script was run with: each
 *  declared name takes the next positional in order, and a `/A` name with
 *  nothing left is an error. `$1..$n` stay the raw arguments. */
bool bind_keys(std::vector<KeySymbol> const &keys, std::string_view script_name,
               std::vector<std::string> const &supplied, Arguments &out,
               std::string &error);

/** Substitute variables and group quotes: the words of a command line.
 *  `$name`, `${name}` and `{name}` substitute a `.KEY` parameter or, under it,
 *  an environment variable; `$0..$n` substitute the script's arguments. `"`
 *  groups words, and a variable inside a group keeps its spaces. A value is
 *  inserted literally, not rescanned. False and fills `error` on an unmatched
 *  quote or brace. */
bool substitute_words(std::string_view line, Arguments const &arguments,
                      Lookup const &variables, std::vector<std::string> &words,
                      std::string &error);

/** An alias table: name, and the raw line it stands for. */
using AliasTable = std::vector<std::pair<std::string, std::string>>;

/** Fold aliases into a line, textually: while its first word names an alias
 *  (case-insensitively), replace that word with the alias's value and go on,
 *  so an alias may name another. A name seen twice is a cycle and stops the
 *  walk -- detection, not a depth cap. The values are raw: their variables are
 *  not expanded here but when the resulting line is substituted, so a variable
 *  in an alias expands when the alias is used, not when it was defined (the
 *  Amiga's textual alias, specs/shell.md). */
std::string expand_aliases(std::string_view line, AliasTable const &aliases);

}  // namespace aegir::script

#endif  // AEGIR_SCRIPT_SUBSTITUTE_H
