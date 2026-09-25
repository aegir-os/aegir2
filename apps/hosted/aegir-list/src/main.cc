/*
 * list: list directories' entries (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * DIR repeats, as the Amiga's Dir/M does, and with none it lists the current
 * directory. Each entry's name, a directory marked with a trailing slash, and
 * a regular file's size; ALL is accepted and adds nothing yet -- there is one
 * listing form -- so the template matches the command's manual.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace {

bool list_one(std::filesystem::path const &dir, uint32_t &count) noexcept
{
    std::error_code error;
    std::filesystem::directory_iterator it(dir, error);
    if (error) {
        return false;
    }
    std::filesystem::directory_iterator const end;
    for (; it != end; it.increment(error)) {
        if (error) {
            return false;
        }
        std::error_code kind;
        bool const directory = it->is_directory(kind);
        bool const regular = it->is_regular_file(kind);
        std::error_code size_error;
        std::uintmax_t const size = regular ? it->file_size(size_error) : 0;
        std::printf("%s%s", it->path().filename().string().c_str(), directory ? "/" : "");
        if (regular && !size_error) {
            std::printf("  %llu", static_cast<unsigned long long>(size));
        }
        std::printf("\n");
        ++count;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("list")) {
        std::_Exit(127);
    }
    aegir::args::Result const args = aegir::args::read("DIR/M,ALL/S", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "list: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    uint32_t count = 0;
    uint32_t const directories = args.count("DIR");
    if (directories == 0) {
        std::error_code error;
        std::filesystem::path const here = std::filesystem::current_path(error);
        if (error || !list_one(here, count)) {
            std::fprintf(stderr, "list: cannot list the current directory\n");
            return 10;
        }
    } else {
        for (uint32_t i = 0; i < directories; ++i) {
            char const *const dir = args.at("DIR", i);
            if (!list_one(dir, count)) {
                std::fprintf(stderr, "list: cannot list %s\n", dir);
                return 10;
            }
        }
    }
    std::printf("%u %s\n", count, count == 1 ? "entry" : "entries");
    return 0;
}
