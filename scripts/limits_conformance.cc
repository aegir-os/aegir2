/*
 * Host conformance for aegir::limits (specs/limits.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * scripts/check_limits.py compiles this with the host compiler against
 * limits.cc and runs it; it is not part of any target build. The parser is a
 * pure value -- no allocation, no libc++, no seL4 -- so the format, the
 * subjects, the amounts and the precedence are asserted exactly.
 */

#include <aegir/limits.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using aegir::limits::Action;
using aegir::limits::Amount;
using aegir::limits::Limits;
using aegir::limits::Resource;
using aegir::limits::View;

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

View named(char const *text)
{
    return View{text, static_cast<uint32_t>(std::strlen(text))};
}

Amount resolve(Limits const &limits, char const *user, char const *klass, Action action)
{
    return limits.resolve(named(user), named(klass), Resource::Memory, action);
}

void expect_amount(Amount const &amount, bool set, uint64_t bytes, char const *what)
{
    ++g_checks;
    if (amount.set != set || (set && amount.bytes != bytes)) {
        ++g_failures;
        std::fprintf(stderr, "FAIL  %s: %s %llu\n", what, amount.set ? "set" : "unset",
                     static_cast<unsigned long long>(amount.bytes));
    }
}

/* The shipped file's shape: comments plus a commented-out default, nothing
 * more. It validates and restricts nobody. */
void check_empty_is_unlimited()
{
    char const *text =
        "# Sys:S/limits.manifest -- resource limits, opt-in (specs/limits.md).\n"
        "#\n"
        "# [default]\n"
        "# memory_log = 64M\n";
    Limits limits;
    expect(limits.parse(text, static_cast<uint32_t>(std::strlen(text))),
           "comments and blanks are a valid file");
    expect_amount(resolve(limits, "u", "default", Action::Log), false, 0,
                  "no rule means unlimited");
    expect_amount(resolve(limits, "u", "default", Action::Deny), false, 0,
                  "no deny rule means unlimited");
}

/* The default applies to a user whose class and name state no rule. */
void check_default_subject()
{
    char const *text = "[default]\nmemory_log = 64M\n";
    Limits limits;
    expect(limits.parse(text, static_cast<uint32_t>(std::strlen(text))), "the default parses");
    expect_amount(resolve(limits, "anyone", "default", Action::Log), true, 64ull << 20,
                  "the default's log applies");
    expect_amount(resolve(limits, "anyone", "desktop", Action::Deny), false, 0,
                  "the default states no deny");
}

/* A class rule applies to every user of that class, and beats the default. */
void check_class_beats_default()
{
    char const *text =
        "[default]\n"
        "memory_log = 64M\n"
        "memory_deny = 1G\n"
        "[class.desktop]\n"
        "memory_deny = 4G\n";
    Limits limits;
    expect(limits.parse(text, static_cast<uint32_t>(std::strlen(text))), "class file parses");
    /* The class states deny only, so log falls back to the default. */
    expect_amount(resolve(limits, "u", "desktop", Action::Log), true, 64ull << 20,
                  "class inherits the default's log");
    expect_amount(resolve(limits, "u", "desktop", Action::Deny), true, 4ull << 30,
                  "class overrides the default's deny");
    expect_amount(resolve(limits, "u", "other", Action::Deny), true, 1ull << 30,
                  "another class keeps the default's deny");
}

/* A user rule beats the user's class. */
void check_user_beats_class()
{
    char const *text =
        "[default]\n"
        "memory_deny = 1G\n"
        "[class.desktop]\n"
        "memory_deny = 4G\n"
        "[user.rroland]\n"
        "memory_deny = 8G\n";
    Limits limits;
    expect(limits.parse(text, static_cast<uint32_t>(std::strlen(text))), "user file parses");
    expect_amount(resolve(limits, "rroland", "desktop", Action::Deny), true, 8ull << 30,
                  "the user beats the class");
    expect_amount(resolve(limits, "someone", "desktop", Action::Deny), true, 4ull << 30,
                  "the class still beats the default");
}

/* A user section with no rule for the pair inherits its class. */
void check_user_inherits_class()
{
    char const *text =
        "[class.desktop]\n"
        "memory_log = 1G\n"
        "[user.rroland]\n"
        "memory_deny = 8G\n";
    Limits limits;
    expect(limits.parse(text, static_cast<uint32_t>(std::strlen(text))), "mixed file parses");
    expect_amount(resolve(limits, "rroland", "desktop", Action::Log), true, 1ull << 30,
                  "the user inherits the class's log");
    expect_amount(resolve(limits, "rroland", "desktop", Action::Deny), true, 8ull << 30,
                  "the user states its own deny");
}

/* K, M and G are binary; no suffix is bytes. */
void check_amounts()
{
    char const *text =
        "[default]\n"
        "memory_log = 5K\n"
        "[class.m]\n"
        "memory_log = 3M\n"
        "[class.g]\n"
        "memory_log = 2G\n"
        "[class.b]\n"
        "memory_log = 4096\n";
    Limits limits;
    expect(limits.parse(text, static_cast<uint32_t>(std::strlen(text))), "amounts parse");
    expect_amount(resolve(limits, "u", "m", Action::Log), true, 3ull << 20, "M is binary");
    expect_amount(resolve(limits, "u", "g", Action::Log), true, 2ull << 30, "G is binary");
    expect_amount(resolve(limits, "u", "b", Action::Log), true, 4096, "no suffix is bytes");
    expect_amount(resolve(limits, "u", "default", Action::Log), true, 5ull << 10, "K is binary");
}

/* Every malformed line is a problem with a line number. */
void expect_problem(char const *text, uint32_t line, char const *what)
{
    Limits limits;
    ++g_checks;
    if (limits.parse(text, static_cast<uint32_t>(std::strlen(text)))) {
        ++g_failures;
        std::fprintf(stderr, "FAIL  %s: parsed\n", what);
        return;
    }
    if (limits.problem().line != line) {
        ++g_failures;
        std::fprintf(stderr, "FAIL  %s: problem at line %u, want %u\n", what,
                     limits.problem().line, line);
    }
}

void check_problems()
{
    expect_problem("[nobody]\nmemory_log = 1M\n", 1, "an unknown section is a problem");
    expect_problem("[class.]\nmemory_log = 1M\n", 1, "an empty class name is a problem");
    expect_problem("[default]\nmemory_kib = 4\n", 2, "an unknown key is a problem");
    expect_problem("[default]\nmemory_log = \n", 2, "an empty amount is a problem");
    expect_problem("[default]\nmemory_log = 1T\n", 2, "an unknown suffix is a problem");
    expect_problem("[default]\nmemory_log = 1M x\n", 2, "trailing text is a problem");
    expect_problem("[default]\nmemory_log = 18446744073709551616\n", 2,
                   "an overflowing amount is a problem");
    expect_problem("[default]\nmemory_log = 1M\nmemory_log = 2M\n", 3,
                   "a repeated key is a problem");
    expect_problem("memory_log = 1M\n", 1, "a key before any section is a problem");
}

}  // namespace

int main()
{
    check_empty_is_unlimited();
    check_default_subject();
    check_class_beats_default();
    check_user_beats_class();
    check_user_inherits_class();
    check_amounts();
    check_problems();

    std::printf("limits: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
