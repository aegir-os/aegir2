/*
 * type: display a text file (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    if (!aegir::command::start("type")) {
        std::_Exit(127);
    }
    aegir::args::Result const args = aegir::args::read("FILE/A", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "type: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    char const *const file = args.value("FILE");
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
    return 0;
}
