/*
 * rename: rename or move files (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * FROM repeats, as the Amiga's From/A/M does. One source renames to TO, or
 * moves into it when TO is a directory; several sources need TO to be a
 * directory and each lands inside it. The volume protocol's rename is
 * same-directory only (specs/vfs.md), so a move into another directory is
 * refused there until it grows a cross-directory rename; a same-directory
 * rename is what the acceptance exercises.
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
    if (!aegir::command::start("rename")) {
        std::_Exit(127);
    }
    aegir::args::Result const args =
        aegir::args::read("FROM/M/A,TO/A", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "rename: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    std::filesystem::path const to = args.value("TO");
    uint32_t const sources = args.count("FROM");
    std::error_code error;
    bool const to_is_directory = std::filesystem::is_directory(to, error);
    if (sources > 1 && !to_is_directory) {
        std::fprintf(stderr, "rename: %s must be a directory for several files\n",
                     to.c_str());
        return 10;
    }
    for (uint32_t i = 0; i < sources; ++i) {
        std::filesystem::path const from = args.at("FROM", i);
        std::filesystem::path destination = to;
        if (to_is_directory) {
            destination = to / from.filename();
        }
        std::filesystem::rename(from, destination, error);
        if (error) {
            std::fprintf(stderr, "rename: cannot rename %s to %s\n", from.c_str(),
                         destination.c_str());
            return 10;
        }
    }
    return 0;
}
