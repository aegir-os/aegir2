/*
 * makedir: create directories (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * NAME repeats, as the Amiga's Name/M does: one line makes several.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

int main(int argc, char **argv)
{
    if (!aegir::command::start("makedir")) {
        std::_Exit(127);
    }
    aegir::args::Result const args = aegir::args::read("NAME/M", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "makedir: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    uint32_t const names = args.count("NAME");
    if (names == 0) {
        std::fprintf(stderr, "makedir: no directory name\nusage: %s\n", args.usage());
        return 10;
    }
    for (uint32_t i = 0; i < names; ++i) {
        char const *const name = args.at("NAME", i);
        std::error_code error;
        if (!std::filesystem::create_directory(name, error)) {
            std::error_code kind;
            if (std::filesystem::is_directory(name, kind)) {
                continue; /* already there: a directory, which is what was asked */
            }
            std::fprintf(stderr, "makedir: cannot create %s\n", name);
            return 10;
        }
    }
    return 0;
}
