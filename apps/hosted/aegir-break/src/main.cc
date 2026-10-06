/*
 * break: set a process's attention flags (specs/process.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The Amiga's Break, a command like any other and a client of
 * process.registry:
 *
 *     Break <process>            set the default flag (C) on the pid
 *     Break <process> ALL|C|D|E|F
 *     Break <process> NAME pat   break every live process whose path, or
 *                                failing that whose name, matches the pattern
 *
 * A pid the registry does not hold, one the caller may not break (the badge
 * rule), or a pattern that matches none is a refusal: the Amiga's WARN band.
 */

#include <aegir/args.h>
#include <aegir/command.h>
#include <aegir/ipc/port.h>
#include <aegir/pattern.h>
#include <aegir/process.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr char const kTemplate[] = "PROCESS/A,NAME/K,ALL/S,C/S,D/S,E/S,F/S";

/* `text` as a decimal pid, or false when it is not one. */
bool parse_pid(char const *text, uint64_t *out) noexcept
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    uint64_t value = 0;
    for (char const *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint64_t>(*p - '0');
    }
    *out = value;
    return true;
}

/* The attention flags the switches asked for; C when none did (the default). */
uint64_t flags_from(aegir::args::Result const &args) noexcept
{
    uint64_t flags = 0;
    if (args.present("ALL")) {
        flags |= aegir::process::kAttnAll;
    }
    if (args.present("C")) {
        flags |= aegir::process::kAttnC;
    }
    if (args.present("D")) {
        flags |= aegir::process::kAttnD;
    }
    if (args.present("E")) {
        flags |= aegir::process::kAttnE;
    }
    if (args.present("F")) {
        flags |= aegir::process::kAttnF;
    }
    return flags != 0 ? flags : aegir::process::kAttnC;
}

/* The length of a row field, bounded by its own width (a Row's name and path
 * are NUL-terminated within their fields). */
uint32_t field_length(char const *field, uint32_t max) noexcept
{
    uint32_t n = 0;
    while (n < max && field[n] != '\0') {
        ++n;
    }
    return n;
}

/* Set `flags` on one pid; true when the registry set them. */
bool set_on(aegir::ipc::Consumer const &registry, uint64_t pid, uint64_t flags) noexcept
{
    uint64_t const words[2] = {pid, flags};
    uint64_t reply[1] = {0};
    aegir::ipc::WordsReply const answer =
        registry.call_words(aegir::process::kMethodBreak, words, 2, reply, 1);
    return answer.error == 0 && answer.count >= 1 && reply[0] == aegir::process::kBreakSet;
}

/* Walk the live set and break every process whose path, or failing that whose
 * name, matches `pattern`. Answer how many were broken. */
uint32_t break_matching(aegir::ipc::Consumer const &registry, char const *pattern,
                        uint64_t flags) noexcept
{
    uint32_t const pattern_length = field_length(pattern, 0xffffffffu);
    uint64_t reply[1] = {0};
    aegir::ipc::WordsReply const total =
        registry.call_words(aegir::process::kMethodCount, nullptr, 0, reply, 1);
    if (total.error != 0 || total.count < 1) {
        return 0;
    }
    uint64_t const count = reply[0];
    uint32_t broken = 0;
    for (uint64_t index = 0; index < count; ++index) {
        uint64_t const in[1] = {index};
        uint64_t row_words[aegir::process::kRowWords] = {};
        aegir::ipc::WordsReply const described = registry.call_words(
            aegir::process::kMethodDescribe, in, 1, row_words, aegir::process::kRowWords);
        if (described.error != 0 || described.count < aegir::process::kRowWords) {
            continue; /* the set moved under us; a walk is a snapshot, not a hold */
        }
        auto const *const row = reinterpret_cast<aegir::process::Row const *>(row_words);
        bool matches = aegir::pattern::match(pattern, pattern_length, row->path,
                                             field_length(row->path, aegir::process::kPathMax));
        if (!matches) {
            matches = aegir::pattern::match(pattern, pattern_length, row->name,
                                            field_length(row->name, aegir::process::kNameMax));
        }
        if (matches && set_on(registry, row->pid, flags)) {
            ++broken;
        }
    }
    return broken;
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("break")) {
        std::_Exit(127);
    }
    aegir::args::Result const args = aegir::args::read(kTemplate, argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stdout, "break: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    aegir::ipc::Consumer const registry = aegir::ipc::Consumer::find(
        aegir::process::kPortName, aegir::process::kPortNameLength);
    if (!registry.valid()) {
        std::fprintf(stdout, "break: no process registry\n");
        return 20;
    }
    uint64_t const flags = flags_from(args);
    char const *const pattern = args.value("NAME");
    if (pattern != nullptr) {
        if (break_matching(registry, pattern, flags) == 0) {
            std::fprintf(stdout, "break: nothing matches %s\n", pattern);
            return 5;
        }
        return 0;
    }
    uint64_t pid = 0;
    if (!parse_pid(args.value("PROCESS"), &pid)) {
        std::fprintf(stdout, "break: %s is not a process id\n", args.value("PROCESS"));
        return 10;
    }
    if (!set_on(registry, pid, flags)) {
        std::fprintf(stdout, "break: process %llu was not broken\n",
                     static_cast<unsigned long long>(pid));
        return 5;
    }
    return 0;
}
