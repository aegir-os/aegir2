/*
 * rename: rename a file or directory (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

int main(int argc, char **argv)
{
    if (!aegir::command::start("rename")) {
        std::_Exit(127);
    }
    aegir::args::Result const args =
        aegir::args::read("FROM/A,TO/A", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "rename: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    std::error_code error;
    std::filesystem::rename(args.value("FROM"), args.value("TO"), error);
    if (error) {
        std::fprintf(stderr, "rename: cannot rename %s to %s\n", args.value("FROM"),
                     args.value("TO"));
        return 10;
    }
    return 0;
}
