/*
 * which: resolve a command name (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A command is a file in the session's C: assignment (specs/dos.md: there is
 * no PATH), so which answers the path it resolves to, and 5 -- the Amiga's
 * warning band -- when the name is not there. Command names are lowercase on
 * Sys:C (the shell lowercases the token), so the lookup does too.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

int main(int argc, char **argv)
{
    if (!aegir::command::start("which")) {
        std::_Exit(127);
    }
    aegir::args::Result const args = aegir::args::read("NAME/A", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "which: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    char const *const name = args.value("NAME");
    std::string path;
    if (std::strchr(name, ':') != nullptr) {
        path = name; /* already a path; resolve it as written */
    } else {
        path = "C:";
        for (char const *c = name; *c != '\0'; ++c) {
            path.push_back(*c >= 'A' && *c <= 'Z' ? static_cast<char>(*c - 'A' + 'a')
                                                  : *c);
        }
    }
    std::error_code error;
    bool const present = std::filesystem::exists(path, error);
    if (error || !present) {
        std::fprintf(stderr, "which: %s not found\n", name);
        return 5;
    }
    std::printf("%s\n", path.c_str());
    return 0;
}
