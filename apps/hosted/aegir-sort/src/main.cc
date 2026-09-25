/*
 * sort: sort a file's lines (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The lines are read whole, sorted, and written to TO; with no TO the file is
 * sorted in place. The order is the Amiga's default, case-insensitive, and
 * byte-for-byte otherwise -- its CASE switch and its locale's collation are
 * not this arc's.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

bool slurp(char const *path, std::string &out) noexcept
{
    std::FILE *const handle = std::fopen(path, "rb");
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

/* Split `text` into lines, dropping the newline that ended each and ignoring a
 * trailing empty one, so a file with or without a final newline sorts the
 * same. */
std::vector<std::string> lines_of(std::string const &text) noexcept
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t const end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

char fold(unsigned char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
}

int compare(std::string const &a, std::string const &b) noexcept
{
    std::size_t const n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        int const left = static_cast<unsigned char>(fold(static_cast<unsigned char>(a[i])));
        int const right = static_cast<unsigned char>(fold(static_cast<unsigned char>(b[i])));
        if (left != right) {
            return left < right ? -1 : 1;
        }
    }
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    return 0;
}

bool write_lines(char const *path, std::vector<std::string> const &lines) noexcept
{
    std::FILE *const handle = std::fopen(path, "wb");
    if (handle == nullptr) {
        return false;
    }
    for (std::string const &line : lines) {
        std::fwrite(line.data(), 1, line.size(), handle);
        std::fputc('\n', handle);
    }
    bool const ok = std::ferror(handle) == 0;
    std::fclose(handle);
    return ok;
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("sort")) {
        std::_Exit(127);
    }
    aegir::args::Result const args = aegir::args::read("FROM/A,TO", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "sort: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    char const *const from = args.value("FROM");
    char const *const to = args.value("TO") != nullptr ? args.value("TO") : from;
    std::string text;
    if (!slurp(from, text)) {
        std::fprintf(stderr, "sort: cannot read %s\n", from);
        return 10;
    }
    std::vector<std::string> lines = lines_of(text);
    /* A stable sort, as the Amiga's is: equal lines keep their order. */
    std::stable_sort(lines.begin(), lines.end(),
                     [](std::string const &a, std::string const &b) {
                         return compare(a, b) < 0;
                     });
    if (!write_lines(to, lines)) {
        std::fprintf(stderr, "sort: cannot write %s\n", to);
        return 10;
    }
    return 0;
}
