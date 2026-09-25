/*
 * join: concatenate files (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Each FROM's bytes are appended, in the order given, to the destination.
 * The destination is a keyword -- the Amiga's AS, which it also spells TO --
 * because FROM repeats: in ReadArgs an /M item takes every remaining bare
 * token, so a bare destination after it could never be reached. At least one
 * FROM and a destination are required, as the Amiga's template has them.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdio>
#include <cstdlib>

namespace {

bool append_file(char const *path, std::FILE *out) noexcept
{
    std::FILE *const handle = std::fopen(path, "rb");
    if (handle == nullptr) {
        return false;
    }
    char buffer[4096];
    std::size_t have = 0;
    bool ok = true;
    while ((have = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) {
        if (std::fwrite(buffer, 1, have, out) != have) {
            ok = false;
            break;
        }
    }
    if (std::ferror(handle) != 0) {
        ok = false;
    }
    std::fclose(handle);
    return ok;
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("join")) {
        std::_Exit(127);
    }
    aegir::args::Result const args =
        aegir::args::read("FROM/M/A,AS/K,TO/K", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "join: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    char const *const to = args.value("AS") != nullptr ? args.value("AS") : args.value("TO");
    if (to == nullptr) {
        std::fprintf(stderr, "join: no destination -- give AS or TO\nusage: %s\n",
                     args.usage());
        return 10;
    }
    std::FILE *const out = std::fopen(to, "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "join: cannot write %s\n", to);
        return 10;
    }
    uint32_t const sources = args.count("FROM");
    for (uint32_t i = 0; i < sources; ++i) {
        char const *const from = args.at("FROM", i);
        if (!append_file(from, out)) {
            std::fprintf(stderr, "join: cannot read %s\n", from);
            std::fclose(out);
            return 10;
        }
    }
    std::fclose(out);
    return 0;
}
