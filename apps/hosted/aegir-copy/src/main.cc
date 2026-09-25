/*
 * copy: copy files or directories (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * FROM repeats, as the Amiga's does. A file copied to a directory keeps its
 * name; a directory needs ALL and copies its tree. With several sources the
 * destination must be a directory. The Amiga's CLONE (cheap links) is not
 * offered: Aegir has no hard-link call yet.
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
    if (!aegir::command::start("copy")) {
        std::_Exit(127);
    }
    aegir::args::Result const args =
        aegir::args::read("FROM/M,TO/A,ALL/S", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "copy: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    std::filesystem::path const to = args.value("TO");
    uint32_t const sources = args.count("FROM");
    std::error_code error;
    bool const to_is_directory = std::filesystem::is_directory(to, error);
    if (sources > 1 && !to_is_directory) {
        std::fprintf(stderr, "copy: %s must be a directory for several files\n", to.c_str());
        return 10;
    }
    for (uint32_t i = 0; i < sources; ++i) {
        std::filesystem::path const from = args.at("FROM", i);
        std::error_code kind;
        bool const from_is_directory = std::filesystem::is_directory(from, kind);
        if (from_is_directory && !args.present("ALL")) {
            std::fprintf(stderr, "copy: %s is a directory -- use ALL\n", from.c_str());
            return 10;
        }
        /* A file copied to a directory keeps its name, as the Amiga's Copy
         * does; a directory copied to a directory lands inside it. */
        std::filesystem::path destination = to;
        if (to_is_directory) {
            destination = to / from.filename();
        }
        std::filesystem::copy_options options =
            std::filesystem::copy_options::overwrite_existing;
        if (from_is_directory) {
            options |= std::filesystem::copy_options::recursive;
        }
        std::filesystem::copy(from, destination, options, error);
        if (error) {
            std::fprintf(stderr, "copy: cannot copy %s to %s\n", from.c_str(),
                         destination.c_str());
            return 10;
        }
    }
    return 0;
}
