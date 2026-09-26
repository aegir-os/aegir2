/*
 * Host conformance for aegir::script (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * scripts/check_script.py compiles this with the host compiler against
 * interpreter.cc and runs it; it is not part of any target build. The
 * command-file parser and the frame stack are values -- no seL4 -- so
 * comment/blank stripping, the frame order, the cycle guard and the fail
 * level are asserted exactly.
 */

#include <aegir/script/interpreter.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

std::vector<std::string> parse(char const *text)
{
    return aegir::script::script_lines(text);
}

/* Comments and blanks are dropped; a kept line keeps its own text. */
void check_lines()
{
    std::vector<std::string> const lines =
        parse("echo one\n; a comment\n\n  echo two\n   ; indented comment\n"
              "echo three\n");
    expect(lines.size() == 3, "three lines survive comments and blanks");
    expect(lines[0] == "echo one", "the first line is kept");
    expect(lines[1] == "  echo two", "a kept line keeps its own leading space");
    expect(lines[2] == "echo three", "the last line is kept");
}

/* A CRLF file reads as an LF one, and a file with no trailing newline keeps
 * its last line. */
void check_crlf_and_tail()
{
    std::vector<std::string> const crlf = parse("echo a\r\necho b\r\n");
    expect(crlf.size() == 2, "CRLF yields two lines");
    expect(crlf[0] == "echo a", "CR is stripped");
    std::vector<std::string> const tail = parse("echo a\necho b");
    expect(tail.size() == 2 && tail[1] == "echo b",
           "a file without a trailing newline keeps its last line");
    std::vector<std::string> const blank = parse("\n\n  \n");
    expect(blank.empty(), "blank-only text has no lines");
}

/* A script's lines come in order, and an exhausted script is dropped. */
void check_run_order()
{
    aegir::script::Frames frames;
    expect(frames.push("S:one", parse("echo a\necho b")), "a script pushes");
    std::string const *line = frames.next();
    expect(line != nullptr && *line == "echo a", "the first line comes first");
    line = frames.next();
    expect(line != nullptr && *line == "echo b", "the next line follows");
    expect(frames.next() == nullptr, "an exhausted script yields the console");
    expect(frames.empty(), "an exhausted script is dropped");
}

/* The innermost script runs first; the outer one resumes when it ends. */
void check_nesting()
{
    aegir::script::Frames frames;
    (void)frames.push("S:outer", parse("echo outer"));
    (void)frames.push("S:inner", parse("echo inner"));
    std::string const *line = frames.next();
    expect(line != nullptr && *line == "echo inner",
           "the innermost script runs first");
    line = frames.next();
    expect(line != nullptr && *line == "echo outer", "the outer script resumes");
}

/* A path already active is a cycle; an empty path (an Eval line) is not. */
void check_cycle()
{
    aegir::script::Frames frames;
    expect(frames.push("S:self", parse("echo a")), "the first push is fine");
    expect(!frames.push("S:self", parse("echo b")), "a repeated path is a cycle");
    expect(frames.push("S:other", parse("echo c")), "a different path is fine");
    expect(frames.push("", parse("echo d")), "an Eval line has no path");
    expect(frames.push("", parse("echo e")), "two Eval lines are not a cycle");
}

/* Quit drops the innermost frame; the outer one is back. */
void check_abort()
{
    aegir::script::Frames frames;
    (void)frames.push("S:outer", parse("echo outer"));
    (void)frames.push("S:inner", parse("echo inner"));
    expect(frames.abort(), "Quit drops the innermost");
    std::string const *line = frames.next();
    expect(line != nullptr && *line == "echo outer", "the outer script is back");
    expect(frames.abort(), "the outer drops too");
    expect(!frames.abort(), "nothing left to drop");
}

/* The Amiga's default level is 10: a warning passes, an error aborts. */
void check_fail_level()
{
    aegir::script::Frames frames;
    expect(frames.fail_level() == 10, "the default level is 10");
    expect(!aegir::script::Frames::fails(0, 10), "success does not fail");
    expect(!aegir::script::Frames::fails(5, 10), "a warning does not abort at 10");
    expect(aegir::script::Frames::fails(10, 10), "an error aborts at 10");
    expect(aegir::script::Frames::fails(20, 10), "a failure aborts at 10");
    expect(!aegir::script::Frames::fails(20, 21), "a higher level passes a 20");
    expect(!aegir::script::Frames::fails(0, 0), "level 0 never aborts, even on 0");
    expect(!aegir::script::Frames::fails(99, 0), "level 0 never aborts");
    frames.set_fail_level(21);
    expect(frames.fail_level() == 21, "the level is settable");
}

}  // namespace

int main()
{
    check_lines();
    check_crlf_and_tail();
    check_run_order();
    check_nesting();
    check_cycle();
    check_abort();
    check_fail_level();

    std::printf("script: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
