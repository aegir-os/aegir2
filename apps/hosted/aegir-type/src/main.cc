/*
 * type: display text files (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * FROM repeats, as the Amiga's From/A/M does: each file's bytes go to the
 * stream in turn. The Amiga's TO, HEX and NUMBER options are not this arc's.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    if (!aegir::command::start("type")) {
        std::_Exit(127);
    }
    aegir::args::Result const args =
        aegir::args::read("FROM/M/A", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "type: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    uint32_t const files = args.count("FROM");
    for (uint32_t i = 0; i < files; ++i) {
        char const *const file = args.at("FROM", i);
        std::FILE *const handle = std::fopen(file, "rb");
        if (handle == nullptr) {
            std::fprintf(stderr, "type: cannot open %s\n", file);
            return 10;
        }
        char buffer[1024];
        std::size_t have = 0;
        while ((have = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) {
            (void)std::fwrite(buffer, 1, have, stdout);
        }
        std::fclose(handle);
    }
    return 0;
}
