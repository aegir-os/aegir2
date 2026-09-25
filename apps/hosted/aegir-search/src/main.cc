/*
 * search: find a string in files (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A file's matching lines go to the stream. A directory needs ALL and is
 * walked, its matches labelled with the file they came from; a single file's
 * matches are not, as the Amiga's Search prints them bare. The match is a
 * substring of the line, byte for byte -- the Amiga's Search is
 * case-insensitive, but its CASE and locale rules are not this arc's.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

bool slurp(std::filesystem::path const &path, std::string &out) noexcept
{
    std::FILE *const handle = std::fopen(path.c_str(), "rb");
    if (handle == nullptr) {
        return false;
    }
    char buffer[4096];
    std::size_t have = 0;
    while ((have = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) {
        out.append(buffer, have);
    }
    bool const ok = std::ferror(handle) == 0;
    std::fclose(handle);
    return ok;
}

void search_file(std::filesystem::path const &path, char const *needle,
                 bool label) noexcept
{
    std::string text;
    if (!slurp(path, text)) {
        return;
    }
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t const end = text.find('\n', start);
        std::string const line =
            text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (line.find(needle) != std::string::npos) {
            if (label) {
                std::printf("%s:", path.filename().string().c_str());
            }
            std::printf("%s\n", line.c_str());
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
}

bool search_tree(std::filesystem::path const &path, char const *needle,
                 std::error_code &error) noexcept
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
        std::error_code kind;
        if (it->is_directory(kind)) {
            if (!search_tree(it->path(), needle, error)) {
                return false;
            }
        } else {
            search_file(it->path(), needle, true);
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("search")) {
        std::_Exit(127);
    }
    aegir::args::Result const args =
        aegir::args::read("FILE/A,SEARCH/A,ALL/S", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "search: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    std::filesystem::path const file = args.value("FILE");
    char const *const needle = args.value("SEARCH");
    std::error_code error;
    if (std::filesystem::is_directory(file, error)) {
        if (!args.present("ALL")) {
            std::fprintf(stderr, "search: %s is a directory -- use ALL\n", file.c_str());
            return 10;
        }
        if (!search_tree(file, needle, error)) {
            std::fprintf(stderr, "search: cannot search %s\n", file.c_str());
            return 10;
        }
    } else if (error) {
        std::fprintf(stderr, "search: cannot find %s\n", file.c_str());
        return 10;
    } else {
        search_file(file, needle, false);
    }
    return 0;
}
