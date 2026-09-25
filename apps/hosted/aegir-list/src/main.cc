/*
 * list: list a directory's entries (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The Amiga's List, in the shape the shell's old built-in Dir had: each
 * entry's name, a directory marked with a trailing slash, and a regular
 * file's size. ALL is accepted and adds nothing yet -- there is one listing
 * form -- so the template matches the command's manual.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

int main(int argc, char **argv)
{
    if (!aegir::command::start("list")) {
        std::_Exit(127);
    }
    aegir::args::Result const args = aegir::args::read("DIR/A,ALL/S", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "list: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    std::error_code error;
    std::filesystem::directory_iterator it(args.value("DIR"), error);
    if (error) {
        std::fprintf(stderr, "list: cannot list %s\n", args.value("DIR"));
        return 10;
    }
    std::filesystem::directory_iterator const end;
    uint32_t count = 0;
    for (; it != end; it.increment(error)) {
        if (error) {
            break;
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
    std::printf("%u %s\n", count, count == 1 ? "entry" : "entries");
    return 0;
}
