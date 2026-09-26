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
#include <aegir/script/substitute.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
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

aegir::script::Lookup lookup_of(std::map<std::string, std::string> table)
{
    return [table = std::move(table)](std::string const &name, std::string &value) {
        auto const it = table.find(name);
        if (it == table.end()) {
            return false;
        }
        value = it->second;
        return true;
    };
}

std::vector<std::string> words_of(std::string const &line,
                                  aegir::script::Arguments const &arguments,
                                  aegir::script::Lookup const &variables,
                                  bool *ok = nullptr)
{
    std::vector<std::string> words;
    std::string error;
    bool const success =
        aegir::script::substitute_words(line, arguments, variables, words, error);
    if (ok != nullptr) {
        *ok = success;
    }
    return words;
}

/* `$name` and `${name}` are environment variables; an undefined one is
 * nothing, not a word (the Amiga's textual substitution). */
void check_substitute_environment()
{
    aegir::script::Arguments const arguments;
    aegir::script::Lookup const variables = lookup_of({{"name", "World"}});
    std::vector<std::string> const words =
        words_of("echo $name ${name} {name}", arguments, variables);
    expect(words.size() == 4, "three expansions are three words");
    expect(words[0] == "echo", "the command word is untouched");
    expect(words[1] == "World" && words[2] == "World" && words[3] == "World",
           "$, ${} and {} all read the variable");
    std::vector<std::string> const missing =
        words_of("echo $nope ${nope}", arguments, variables);
    expect(missing.size() == 1, "an undefined variable adds no word");
    std::vector<std::string> const lone =
        words_of("echo $ 100$", arguments, variables);
    expect(lone.size() == 3 && lone[1] == "$" && lone[2] == "100$",
           "a lone $ is literal");
}

/* A quote groups a word; a variable inside it keeps its spaces, and outside
 * it the value splits on them. */
void check_substitute_quotes()
{
    aegir::script::Arguments const arguments;
    aegir::script::Lookup const variables = lookup_of({{"spaced", "two words"}});
    std::vector<std::string> const bare = words_of("echo $spaced", arguments, variables);
    expect(bare.size() == 3 && bare[1] == "two" && bare[2] == "words",
           "an unquoted value splits on its spaces");
    std::vector<std::string> const quoted =
        words_of("echo \"$spaced\"", arguments, variables);
    expect(quoted.size() == 2 && quoted[1] == "two words",
           "a quoted value keeps its spaces");
    std::vector<std::string> const empty = words_of("echo \"\"", arguments, variables);
    expect(empty.size() == 2 && empty[1].empty(), "an empty quote is an empty word");
    std::vector<std::string> const mixed = words_of("echo a\"b c\"d", arguments, variables);
    expect(mixed.size() == 2 && mixed[1] == "ab cd",
           "a quote groups within a word");
}

/* `$0..$n` are the script's arguments; {name} is a .KEY binding. */
void check_substitute_arguments()
{
    aegir::script::Arguments arguments;
    arguments.name = "S:test";
    arguments.positional = {"a", "b"};
    arguments.named = {{"who", "World"}};
    aegir::script::Lookup const variables = lookup_of({});
    std::vector<std::string> const words =
        words_of("hello {who} $1 $2 $3 $0", arguments, variables);
    expect(words.size() == 5 && words[1] == "World", "{name} reads a .KEY binding");
    expect(words[2] == "a" && words[3] == "b", "$1..$n are the arguments");
    expect(words[4] == "S:test", "$0 is the script's name");
    std::vector<std::string> const absent =
        words_of("echo $9", arguments, variables);
    expect(absent.size() == 1, "an absent argument adds no word");
}

/* An unmatched quote or brace is an error, not a silent mangling. */
void check_substitute_errors()
{
    aegir::script::Arguments const arguments;
    aegir::script::Lookup const variables = lookup_of({});
    bool ok = true;
    (void)words_of("echo \"open", arguments, variables, &ok);
    expect(!ok, "an unmatched quote is refused");
    (void)words_of("echo {open", arguments, variables, &ok);
    expect(!ok, "an unmatched brace is refused");
}

/* A .KEY directive declares the names a script's arguments bind to. */
void check_key_lines()
{
    expect(aegir::script::is_key_line(".KEY a/A"), ".KEY is a directive");
    expect(aegir::script::is_key_line("   .key b"), "leading blanks and case are fine");
    expect(!aegir::script::is_key_line(".KEYSTONE"), "a longer word is not .KEY");
    expect(!aegir::script::is_key_line("echo .KEY"), "a .KEY not first is not a directive");

    std::vector<aegir::script::KeySymbol> keys;
    std::string error;
    expect(aegir::script::parse_key_list(".KEY a/A,b", keys, error),
           "a list of names parses");
    expect(keys.size() == 2 && keys[0].name == "a" && keys[0].required,
           "the /A name is required");
    expect(keys[1].name == "b" && !keys[1].required, "a bare name is optional");
    expect(!aegir::script::parse_key_list(".KEY a/S", keys, error),
           "an unsupported flag is refused");
    expect(!error.empty(), "the refusal says why");

    aegir::script::KeySymbol const declared[] = {{"a", true}, {"b", false}};
    aegir::script::Arguments bound;
    expect(aegir::script::bind_keys({declared[0], declared[1]}, "S:x", {"1"},
                                    bound, error),
           "one argument fills the required name");
    expect(bound.named.size() == 2 && bound.named[0].second == "1",
           "the first name takes the first argument");
    expect(bound.named[1].second.empty(), "an unmet optional name is empty");
    expect(bound.positional.size() == 1 && bound.positional[0] == "1",
           "the raw arguments are kept for $1");
    expect(!aegir::script::bind_keys({declared[0]}, "S:x", {}, bound, error),
           "a required name with nothing left is refused");
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
    check_substitute_environment();
    check_substitute_quotes();
    check_substitute_arguments();
    check_substitute_errors();
    check_key_lines();

    std::printf("script: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
