/*
 * Host conformance for aegir::args (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * scripts/check_args.py compiles this with the host compiler against
 * args.cc and runs it; it is not part of any target build. The parser is a
 * pure value -- no allocation, no libc++, no seL4 -- so its template reading,
 * its positional order, and the /M item's reservation of a later positional
 * are asserted exactly.
 */

#include <aegir/args.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_checks = 0;
int g_failures = 0;

void expect(bool condition, char const *what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL  %s\n", what);
    }
}

void expect_value(aegir::args::Result const &r, char const *name, char const *want,
                  char const *what)
{
    char const *const got = r.value(name);
    ++g_checks;
    if (got == nullptr || std::strcmp(got, want) != 0) {
        ++g_failures;
        std::fprintf(stderr, "FAIL  %s: %s = \"%s\" want \"%s\"\n", what, name,
                     got != nullptr ? got : "(absent)", want);
    }
}

/* FROM/A,TO/A: two positionals in order. */
void check_positional_pair()
{
    char const *argv[] = {"a", "b"};
    aegir::args::Result const r = aegir::args::read("FROM/A,TO/A", 2, argv);
    expect(r.ok(), "FROM/A,TO/A a b is well-formed");
    expect_value(r, "FROM", "a", "FROM/A,TO/A");
    expect_value(r, "TO", "b", "FROM/A,TO/A");
}

/* FROM/M,TO/A: the /M item leaves the trailing positional its argument. */
void check_multiple_reserves_trailing()
{
    char const *argv[] = {"a", "b", "c"};
    aegir::args::Result const r = aegir::args::read("FROM/M,TO/A", 3, argv);
    expect(r.ok(), "FROM/M,TO/A a b c is well-formed");
    expect(r.count("FROM") == 2, "FROM/M takes two of the three");
    expect_value(r, "FROM", "a", "FROM/M first");
    expect_value(r, "TO", "c", "FROM/M leaves TO the last");
    char const *const second = r.at("FROM", 1);
    expect(second != nullptr && std::strcmp(second, "b") == 0, "FROM/M second is b");
}

/* A switch between the /M item and the trailing positional is not a
 * positional token and must not consume the reservation. */
void check_multiple_skips_switch()
{
    char const *argv[] = {"a", "b", "ALL", "c"};
    aegir::args::Result const r = aegir::args::read("FROM/M,TO/A,ALL/S", 4, argv);
    expect(r.ok(), "FROM/M,TO/A,ALL/S a b ALL c is well-formed");
    expect(r.count("FROM") == 2, "FROM/M skips the switch");
    expect(r.present("ALL"), "ALL is present");
    expect_value(r, "TO", "c", "TO is c past the switch");
}

/* The /M item is last: it takes every remaining bare token. */
void check_multiple_last()
{
    char const *argv[] = {"a", "b", "ALL"};
    aegir::args::Result const r = aegir::args::read("FILE/M/A,ALL/S", 3, argv);
    expect(r.ok(), "FILE/M/A,ALL/S a b ALL is well-formed");
    expect(r.count("FILE") == 2, "FILE/M takes both");
    expect(r.present("ALL"), "ALL is present");
}

/* One token for two positionals: the required one is missing. */
void check_required_missing()
{
    char const *argv[] = {"a"};
    aegir::args::Result const r = aegir::args::read("FROM/M,TO/A", 1, argv);
    expect(!r.ok(), "FROM/M,TO/A a is missing TO");
    expect(r.missing != nullptr && std::strncmp(r.missing, "TO", 2) == 0,
           "the missing name is TO");
}

/* A keyword is reachable after a /M item, and its value is the next token. */
void check_keyword_after_multiple()
{
    char const *argv[] = {"a", "b", "AS", "c"};
    aegir::args::Result const r = aegir::args::read("FROM/M/A,AS/K,TO/K", 4, argv);
    expect(r.ok(), "join a b AS c is well-formed");
    expect(r.count("FROM") == 2, "FROM/M takes two");
    expect_value(r, "AS", "c", "AS is the keyword's value");
    expect(!r.present("TO"), "TO is absent");
}

/* The Amiga's alias spelling: TO names the same destination. */
void check_keyword_alias()
{
    char const *argv[] = {"a", "b", "TO", "c"};
    aegir::args::Result const r = aegir::args::read("FROM/M/A,AS/K,TO/K", 4, argv);
    expect(r.ok(), "join a b TO c is well-formed");
    expect(r.count("FROM") == 2, "FROM/M takes two");
    expect_value(r, "TO", "c", "TO is the keyword's value");
    expect(!r.present("AS"), "AS is absent");
}

/* search: FROM/M then the required SEARCH. */
void check_search_shape()
{
    char const *argv[] = {"d1", "d2", "text"};
    aegir::args::Result const r = aegir::args::read("FROM/M,SEARCH/A,ALL/S", 3, argv);
    expect(r.ok(), "search d1 d2 text is well-formed");
    expect(r.count("FROM") == 2, "FROM/M takes two");
    expect_value(r, "SEARCH", "text", "SEARCH is the last");
}

/* NAME=VALUE names an item outright, whatever its position. */
void check_named_values()
{
    char const *argv[] = {"FROM=x", "TO=y"};
    aegir::args::Result const r = aegir::args::read("FROM/A,TO/A", 2, argv);
    expect(r.ok(), "FROM=x TO=y is well-formed");
    expect_value(r, "FROM", "x", "FROM named");
    expect_value(r, "TO", "y", "TO named");
}

/* A switch is present or absent; a bare token with no item to fill is an
 * unknown argument. */
void check_switch_and_unknown()
{
    char const *on[] = {"ALL"};
    aegir::args::Result const present = aegir::args::read("ALL/S", 1, on);
    expect(present.ok(), "ALL is a well-formed switch");
    expect(present.present("ALL"), "ALL is present");

    char const *none[] = {""};
    aegir::args::Result const absent = aegir::args::read("ALL/S", 0, none);
    expect(absent.ok(), "no switch is well-formed");
    expect(!absent.present("ALL"), "ALL is absent");

    char const *bad[] = {"--nope"};
    aegir::args::Result const unknown = aegir::args::read("ALL/S", 1, bad);
    expect(!unknown.ok(), "a bare token with no item is unknown");
}

/* The /M item with exactly one token, then the trailing positional. */
void check_multiple_one_token()
{
    char const *argv[] = {"a", "b"};
    aegir::args::Result const r = aegir::args::read("FROM/M,TO/A", 2, argv);
    expect(r.ok(), "FROM/M,TO/A a b is well-formed");
    expect(r.count("FROM") == 1, "FROM/M takes one");
    expect_value(r, "FROM", "a", "FROM is a");
    expect_value(r, "TO", "b", "TO is b");
}

}  // namespace

int main()
{
    check_positional_pair();
    check_multiple_reserves_trailing();
    check_multiple_skips_switch();
    check_multiple_last();
    check_required_missing();
    check_keyword_after_multiple();
    check_keyword_alias();
    check_search_shape();
    check_named_values();
    check_switch_and_unknown();
    check_multiple_one_token();

    std::printf("args: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
