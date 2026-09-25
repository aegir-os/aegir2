/*
 * copy: copy files or directories (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A file copies to a name, or into a directory (the source's name is kept).
 * A directory needs ALL, and copies its tree. The Amiga's CLONE (cheap links)
 * is not offered: Aegir has no hard-link call yet.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

int main(int argc, char **argv)
{
    if (!aegir::command::start("copy")) {
        std::_Exit(127);
    }
    aegir::args::Result const args =
        aegir::args::read("FROM/A,TO/A,ALL/S", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "copy: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    std::filesystem::path const from = args.value("FROM");
    std::filesystem::path to = args.value("TO");
    std::error_code error;
    bool const from_is_directory = std::filesystem::is_directory(from, error);

    /* A file copied to a directory keeps its name, as the Amiga's Copy does. */
    if (!from_is_directory && std::filesystem::is_directory(to, error)) {
        to /= from.filename();
    }

    std::filesystem::copy_options options = std::filesystem::copy_options::overwrite_existing;
    if (from_is_directory) {
        if (!args.present("ALL")) {
            std::fprintf(stderr, "copy: %s is a directory -- use ALL\n", from.c_str());
            return 10;
        }
        options |= std::filesystem::copy_options::recursive;
    }
    std::filesystem::copy(from, to, options, error);
    if (error) {
        std::fprintf(stderr, "copy: cannot copy %s to %s\n", from.c_str(), to.c_str());
        return 10;
    }
    return 0;
}
