/*
 * dir: list a directory's entry names (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The Amiga's Dir: the names alone, a directory marked with a trailing slash,
 * where List shows the detail. DIR repeats, and with none it lists the current
 * directory; ALL is accepted and adds nothing yet, as List's does.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace {

bool dir_one(std::filesystem::path const &directory, uint32_t &count) noexcept
{
    std::error_code error;
    std::filesystem::directory_iterator it(directory, error);
    if (error) {
        return false;
    }
    std::filesystem::directory_iterator const end;
    for (; it != end; it.increment(error)) {
        if (error) {
            return false;
        }
        std::error_code kind;
        bool const is_directory = it->is_directory(kind);
        std::printf("%s%s\n", it->path().filename().string().c_str(),
                    is_directory ? "/" : "");
        ++count;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("dir")) {
        std::_Exit(127);
    }
    aegir::args::Result const args = aegir::args::read("DIR/M,ALL/S", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "dir: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    uint32_t count = 0;
    uint32_t const directories = args.count("DIR");
    if (directories == 0) {
        std::error_code error;
        std::filesystem::path const here = std::filesystem::current_path(error);
        if (error || !dir_one(here, count)) {
            std::fprintf(stderr, "dir: cannot list the current directory\n");
            return 10;
        }
    } else {
        for (uint32_t i = 0; i < directories; ++i) {
            char const *const directory = args.at("DIR", i);
            if (!dir_one(directory, count)) {
                std::fprintf(stderr, "dir: cannot list %s\n", directory);
                return 10;
            }
        }
    }
    std::printf("%u %s\n", count, count == 1 ? "entry" : "entries");
    return 0;
}
