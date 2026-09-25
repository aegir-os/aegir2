/*
 * delete: delete files or directories (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * FILE repeats, as the Amiga's File/M/A does. A directory needs ALL, and its
 * whole tree goes. The walk is the command's own, down the paths a
 * directory_iterator hands out, rather than std::filesystem::remove_all's:
 * that one walks through directory file descriptors, which the runtime's *at
 * calls do not anchor on yet. FORCE is accepted, for the Amiga's name, and
 * changes nothing yet.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace {

bool remove_tree(std::filesystem::path const &path, std::error_code &error) noexcept
{
    std::filesystem::directory_iterator it(path, error);
    if (error) {
        return false;
    }
    std::filesystem::directory_iterator const end;
    for (; it != end; it.increment(error)) {
        if (error) {
            return false;
        }
        std::filesystem::path const child = it->path();
        std::error_code kind;
        if (it->is_directory(kind)) {
            if (!remove_tree(child, error)) {
                return false;
            }
        } else if (!std::filesystem::remove(child, error) || error) {
            return false;
        }
    }
    return std::filesystem::remove(path, error) && !error;
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("delete")) {
        std::_Exit(127);
    }
    aegir::args::Result const args =
        aegir::args::read("FILE/M/A,ALL/S,FORCE/S", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "delete: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    uint32_t const files = args.count("FILE");
    for (uint32_t i = 0; i < files; ++i) {
        char const *const file = args.at("FILE", i);
        std::error_code error;
        bool const directory = std::filesystem::is_directory(file, error);
        if (error) {
            std::fprintf(stderr, "delete: cannot find %s\n", file);
            return 10;
        }
        if (directory) {
            if (!args.present("ALL")) {
                std::fprintf(stderr, "delete: %s is a directory -- use ALL\n", file);
                return 10;
            }
            if (!remove_tree(file, error)) {
                std::fprintf(stderr, "delete: cannot delete %s\n", file);
                return 10;
            }
        } else if (!std::filesystem::remove(file, error) || error) {
            std::fprintf(stderr, "delete: cannot delete %s\n", file);
            return 10;
        }
    }
    return 0;
}
