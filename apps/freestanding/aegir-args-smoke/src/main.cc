/*
 * aegir-args-smoke: the ReadArgs library's acceptance client (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * It runs the parse cases the DOS toolset leans on -- a positional and a
 * switch, a case-blind name, a keyword, a missing required argument, a number,
 * and a repeatable item -- and prints ARGS_SMOKE_OK when every one answers. It
 * is freestanding: nothing in aegir::args needs libc++ or seL4, and the proof
 * that it does not is a smoke that links neither.
 */

#include <aegir/args.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

namespace {

bool g_failed = false;

void expect(bool condition, char const *what) noexcept
{
    if (condition) {
        return;
    }
    g_failed = true;
    aegir::debug_write("  args-smoke: FAIL ");
    aegir::debug_write(what);
    aegir::debug_write("\n");
}

bool same(char const *a, char const *b) noexcept
{
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    uint32_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return false;
        }
        ++i;
    }
    return a[i] == b[i];
}

void run_cases() noexcept
{
    /* A positional argument, an optional one, and a switch. */
    {
        char const *const line[] = {"source", "dest", "ALL"};
        aegir::args::Result const r =
            aegir::args::read("FROM/A,TO,ALL/S", 3, line);
        expect(r.ok(), "a well-formed line is accepted");
        expect(same(r.value("FROM"), "source"), "FROM is the first positional");
        expect(same(r.value("TO"), "dest"), "TO is the next positional");
        expect(r.present("ALL"), "ALL is a switch that was given");
        expect(same(r.value("ALL"), ""), "a switch has no value");
    }

    /* Names are case-blind, and NAME=VALUE names a value exactly. */
    {
        char const *const line[] = {"from=source", "to=dest"};
        aegir::args::Result const r =
            aegir::args::read("FROM/A,TO", 2, line);
        expect(r.ok(), "NAME=VALUE is accepted");
        expect(same(r.value("from"), "source"), "a lowercased name matches");
    }

    /* A keyword: the name, then its value. */
    {
        char const *const line[] = {"file", "NAME", "title"};
        aegir::args::Result const r =
            aegir::args::read("FILE/A,NAME/K", 3, line);
        expect(r.ok(), "a keyword line is accepted");
        expect(same(r.value("FILE"), "file"), "the positional is filled");
        expect(same(r.value("NAME"), "title"), "the keyword takes the next token");
    }

    /* A missing required argument is refused, and named. */
    {
        char const *const line[] = {"only"};
        aegir::args::Result const r =
            aegir::args::read("FROM/A,TO/A", 1, line);
        expect(!r.ok(), "a missing required argument is refused");
        expect(r.missing != nullptr && r.missing_length == 2 &&
                   r.missing[0] == 'T' && r.missing[1] == 'O',
               "the missing argument is named");
    }

    /* A number must be one. */
    {
        char const *const good[] = {"12"};
        expect(aegir::args::read("N/A/N", 1, good).ok(), "a number is accepted");
        char const *const bad[] = {"one"};
        expect(!aegir::args::read("N/A/N", 1, bad).ok(), "a non-number is refused");
    }

    /* A repeatable item keeps every value, in order. */
    {
        char const *const line[] = {"a", "b", "c"};
        aegir::args::Result const r = aegir::args::read("X/M", 3, line);
        expect(r.ok(), "a repeatable line is accepted");
        expect(r.count("X") == 3, "all three values are kept");
        expect(same(r.at("X", 1), "b"), "the second value is in order");
    }
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);
    aegir::debug_write("\nargs-smoke: the ReadArgs library\n");
    run_cases();
    aegir::debug_write(g_failed ? "ARGS_SMOKE_FAIL\n" : "ARGS_SMOKE_OK\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
    return 0;
}
