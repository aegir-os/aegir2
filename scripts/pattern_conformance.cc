/*
 * Host conformance for the AmigaDOS matcher (specs/pattern.md).
 *
 * The matcher is a pure predicate: a pattern and a name in, true or false out,
 * with no allocation and no state. Synthetic strings are enough, and the cases
 * a screen shows only by luck -- an empty pattern, a bare #?, an unterminated
 * ( or [, a proper prefix -- are exactly the ones worth pinning.
 *
 * Host tools only (python3 and a C++ compiler); not part of any target build.
 * Run through scripts/check_pattern.py.
 */

#include <aegir/pattern.h>

#include <cstdio>
#include <string>

namespace {

using aegir::pattern::match;

unsigned g_checks = 0;
unsigned g_failures = 0;

void expect(char const *pattern, char const *name, bool want, std::string const &what) {
    ++g_checks;
    bool const got = match(pattern, static_cast<uint32_t>(std::char_traits<char>::length(pattern)),
                           name, static_cast<uint32_t>(std::char_traits<char>::length(name)));
    if (got != want) {
        ++g_failures;
        std::printf("FAIL %s: %s vs %s, got %s\n", what.c_str(), pattern, name,
                    got ? "match" : "no match");
    }
}

/* The case the eye reads: a name a pattern should keep. */
void keeps(char const *pattern, char const *name, std::string const &what) {
    expect(pattern, name, true, what);
}

/* And one it should not. */
void drops(char const *pattern, char const *name, std::string const &what) {
    expect(pattern, name, false, what);
}

}  // namespace

int main() {
    /* A pattern with no token in it matches only itself, case-insensitively. */
    keeps("foo", "foo", "a literal matches itself");
    keeps("foo", "FOO", "the match folds case");
    drops("foo", "foobar", "a literal is the whole name");
    drops("foo", "fo", "a literal is not a prefix");

    /* ? -- any single character. */
    keeps("f?o", "foo", "? takes one character");
    drops("f?o", "fo", "? needs its character");
    drops("f?o", "fooo", "? takes exactly one");
    keeps("?", "a", "a lone ? is one character");
    drops("?", "", "a lone ? needs a character");

    /* #X -- zero or more of X. */
    keeps("A#BC", "AC", "# takes zero");
    keeps("A#BC", "ABC", "# takes one");
    keeps("A#BC", "ABBC", "# takes many");
    drops("A#BC", "AB", "# is not a wildcard");
    drops("A#BC", "AXC", "# repeats its own expression");
    keeps("#(AB)", "", "#(AB) takes zero groups");
    keeps("#(AB)", "ABAB", "#(AB) repeats a group");
    drops("#(AB)", "ABA", "#(AB) is whole groups");

    /* #? -- any run, the empty string included. */
    keeps("#?.prefs", "dirkf.prefs", "#? takes a run");
    keeps("#?.prefs", ".prefs", "#? takes an empty run");
    drops("#?.prefs", "prefs", "#? does not invent the dot");
    keeps("#?", "abc", "a bare #? takes any name");
    keeps("#?", "", "a bare #? takes the empty name");
    keeps("foo#?", "foobar", "#? after a literal takes the tail");
    keeps("#?bar", "foobar", "#? before a literal takes the head");

    /* (a|b) -- one of the alternatives. */
    keeps("A(B|C)D", "ABD", "an alternative takes the first");
    keeps("A(B|C)D", "ACD", "an alternative takes the second");
    drops("A(B|C)D", "AD", "an alternative needs one");
    drops("A(B|C)D", "ABCD", "an alternative is one, not a run");

    /* % -- the empty string. */
    keeps("A(B|%)C", "ABC", "the B alternative");
    keeps("A(B|%)C", "AC", "% is the empty alternative");
    drops("A(B|%)C", "ADC", "no alternative matches D");

    /* ~X -- anything X does not match. */
    keeps("~(foo)", "foobar", "~ takes a longer name");
    drops("~(foo)", "foo", "~ rejects what X matches");
    keeps("~(#?.info)", "foo", "~ keeps a name not ending .info");
    drops("~(#?.info)", "foo.info", "~ drops a name ending .info");
    keeps("~x?", "ab", "~x takes one non-x character");
    drops("~x?", "xb", "~x rejects x");

    /* [abc] and [a-c] -- character classes and ranges. */
    keeps("[abc]x", "ax", "a class takes a member");
    keeps("[abc]x", "cx", "a class takes its last member");
    drops("[abc]x", "dx", "a class rejects a non-member");
    keeps("[a-c]x", "bx", "a range takes a member");
    drops("[a-c]x", "dx", "a range rejects outside it");
    keeps("[A-C]x", "bx", "a range folds case");
    keeps("[~abc]x", "dx", "a negated class takes a non-member");
    drops("[~abc]x", "ax", "a negated class rejects a member");

    /* 'c -- the literal c. */
    keeps("'?x", "?x", "a quote makes ? literal");
    drops("'?x", "ax", "a quoted ? is not a wildcard");
    keeps("''", "'", "a quote makes ' literal");

    /* * is not a wildcard: AmigaDOS's RNF_WILDSTAR option, off. */
    keeps("*x", "*x", "* is a literal star");
    drops("*x", "ax", "* does not match a run");

    /* The edges a screen shows only by luck. */
    keeps("", "", "an empty pattern takes the empty name");
    drops("", "a", "an empty pattern takes nothing else");
    drops("(ab", "ab", "an unterminated group matches nothing");
    drops("[ab", "a", "an unterminated class matches nothing");

    std::printf("pattern: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
