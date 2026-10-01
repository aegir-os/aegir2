# pattern: AmigaDOS wildcards

Status: decided (2026-10). The matcher the shell's globbing, `dir`/`list` and
the file requester share (`specs/trinket/file_requester.md`).

## The problem

An AmigaDOS command line filters names with a pattern -- `#?.prefs`, `foo?`,
`(a|b)` -- and nothing in the tree matches one. The shell's globbing is
deferred (`specs/shell.md`), `dir`/`list` list every entry, and a file
requester's Pattern box is the first client that needs it. Writing a second
matcher inside the requester would be two behaviours to keep in step.

## The decisions

- **The syntax is AmigaDOS's.** The tokens are `dos.library`'s
  `ParsePattern`/`MatchPattern` ones, not POSIX's:

  | token | matches |
  | --- | --- |
  | `?` | any single character |
  | `#X` | zero or more repetitions of the expression `X` |
  | `#?` | any run of characters, the empty string included |
  | `(a|b)` | any one of the alternatives, `|`-separated |
  | `~X` | anything that does not match `X` |
  | `[abc]` | any one character in the class |
  | `[~abc]` | any one character not in the class |
  | `a-z` | a character range, only inside a class |
  | `%` | the empty string, always |
  | `'c` | the literal `c`, whatever `c` is |

  An *expression* is one character (`#?`), an alternation (`#(ab|cd)`) or a
  class (`#[a-z]`); `#`, `~` and the alternatives apply to one. Everything
  else is a literal.

- **`*` is not a wildcard.** AmigaDOS makes it a synonym for `#?` only under
  the `RNF_WILDSTAR` option and discourages it (a bare `*` is the console,
  not a pattern). Aegir leaves it literal, and `#?` is the wildcard, as the
  Amiga's own examples use it (`DELETE #?.info`).

- **Matching is case-insensitive**, as `MatchPatternNoCase` is and as
  AmigaDOS filenames are. One `match` is the case-insensitive one; a
  case-sensitive variant waits for a caller that wants it.

- **One library, and it is pure.** `libs/freestanding/aegir-pattern`
  (`aegir/pattern.h`) has no seL4, no libc and no allocation, so the shell,
  `dir`/`list`, the toolkit and a filesystem can all link it -- the same
  place `aegir-args`, the other pure parser, lives. It compiles for the host
  too, so `make check-pattern` pins it.

- **Matching, not walking.** `match` answers "does this one name match this
  pattern". It does not open a directory, expand a pattern into a list, or
  split a path: a caller that wants the entries lists the directory and
  keeps the names `match` accepts. The file requester does exactly that,
  and so will `dir`/`list`.

## The shape

```cpp
// pattern.h
namespace aegir::pattern {

/** True when `name` matches the AmigaDOS `pattern`, case-insensitively. A
 *  pattern with no token in it matches only itself. */
bool match(char const *pattern, uint32_t pattern_length,
           char const *name, uint32_t name_length) noexcept;

}
```

## What this is not

- **A regular expression.** No anchors, no `+`/`{}` repetition, no POSIX
  class extensions beyond AmigaDOS's own `[a-z]` and `[~...]`.
- **`*` as a wildcard.** AmigaDOS's `RNF_WILDSTAR` option, deliberately off
  (above).
- **A directory walker.** `match` is a predicate. AmigaDOS's
  `MatchFirst`/`MatchNext` -- pattern-driven directory scanning -- are not
  part of this; a caller lists and filters.
- **A tokenizer API.** AmigaDOS splits parse from match to scan many names
  quickly. `match` reparses each call, which a directory of a few hundred
  names does not feel; a parsed form waits for a caller that measures it.
- **POSIX bracket classes.** `[[:alpha:]]` and friends are not AmigaDOS and
  are not here.

## Acceptance

- **`make check-pattern`** (`scripts/check_pattern.py` +
  `scripts/pattern_conformance.cc`): the tokens, the case folding, the
  escape, and the cases a screen shows only by luck -- an empty pattern, a
  bare `#?`, a trailing `?`, an unterminated `(`, an unterminated `[`, a
  class range, a negated class, and a name that is a proper prefix of the
  pattern.
- **The file requester's Pattern box** is the first client
  (`specs/trinket/file_requester.md`); the shell's globbing and `dir`/`list`
  follow.
