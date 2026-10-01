/*
 * AmigaDOS wildcards (specs/pattern.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The one matcher the shell's globbing, `dir`/`list` and a file requester's
 * Pattern box share. The syntax is `dos.library`'s `ParsePattern`/`MatchPattern`
 * tokens, not POSIX's:
 *
 *   ?        any single character
 *   #X       zero or more repetitions of the expression X
 *   #?       any run of characters, the empty string included
 *   (a|b)    any one of the alternatives
 *   ~X       anything that does not match X
 *   [abc]    any one character in the class
 *   [~abc]   any one character not in the class
 *   a-z      a character range, only inside a class
 *   %        the empty string, always
 *   'c       the literal c
 *
 * Everything else is a literal. `*` is *not* a wildcard: AmigaDOS makes it a
 * synonym for `#?` only under the discouraged `RNF_WILDSTAR` option, so `#?`
 * is the wildcard. Matching is case-insensitive, as `MatchPatternNoCase` is.
 *
 * Pure: no allocation, no libc, no seL4. `match` reparses the pattern on each
 * call; a directory of a few hundred names does not feel it, and a parsed form
 * waits for a caller that measures one (specs/pattern.md).
 */

#ifndef AEGIR_PATTERN_H
#define AEGIR_PATTERN_H

#include <stdint.h>

namespace aegir::pattern {

/** True when `name` matches the AmigaDOS `pattern`, case-insensitively. A
 *  pattern with no token in it matches only itself. */
bool match(char const *pattern, uint32_t pattern_length,
           char const *name, uint32_t name_length) noexcept;

}  // namespace aegir::pattern

#endif  // AEGIR_PATTERN_H
