/*
 * version: report a file's version (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The Amiga's version string is a "$VER:" line inside the file, so the
 * command scans the bytes for the marker and prints the rest of that line.
 * A file without one is reported and returns 5 -- the Amiga's warning band.
 * The scan carries its match across read boundaries, so a marker split by a
 * buffer edge is still found.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr char kMarker[] = "$VER:";
constexpr uint32_t kMarkerLength = sizeof(kMarker) - 1;

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("version")) {
        std::_Exit(127);
    }
    aegir::args::Result const args = aegir::args::read("FILE", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "version: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    char const *const file = args.value("FILE");
    if (file == nullptr) {
        std::fprintf(stderr, "version: no file given\nusage: %s\n", args.usage());
        return 10;
    }
    std::FILE *const handle = std::fopen(file, "rb");
    if (handle == nullptr) {
        std::fprintf(stderr, "version: cannot open %s\n", file);
        return 10;
    }
    char buffer[512];
    uint32_t matched = 0;
    bool collecting = false;
    bool found = false;
    std::string text;
    std::size_t have = 0;
    while (!found && (have = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) {
        for (std::size_t i = 0; i < have; ++i) {
            char const c = buffer[i];
            if (!collecting) {
                if (c == kMarker[matched]) {
                    if (++matched == kMarkerLength) {
                        collecting = true;
                        text.clear();
                    }
                } else {
                    matched = c == kMarker[0] ? 1 : 0;
                }
            } else if (c == '\n') {
                found = true;
                break;
            } else if (c != '\r') {
                text.push_back(c);
            }
        }
    }
    std::fclose(handle);
    if (collecting) {
        found = true; /* the marker reached the end of the file */
    }
    if (!found) {
        std::fprintf(stderr, "version: no version string in %s\n", file);
        return 5;
    }
    std::printf("%s%s\n", kMarker, text.c_str());
    return 0;
}
