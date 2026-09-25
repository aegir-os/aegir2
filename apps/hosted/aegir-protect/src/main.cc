/*
 * protect: change a file's protection bits (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The Amiga's protection letters on our POSIX mode model: r, w and e set the
 * read, write and execute bits for every class (r is 0444, w 0222, e 0111),
 * and the letters given replace the file's permission bits. d, s and p have
 * no POSIX bit yet and are accepted but stored nowhere (specs/bfs.md decision
 * 7). Only the owner or the system class may change a mode, which the
 * filesystem enforces; a filesystem with no modes -- FAT -- answers
 * EOPNOTSUPP.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace {

/* The Amiga letters as POSIX bits: each sets its class triad, and an absent
 * letter leaves that bit clear (Protect replaces, as the Amiga's does). */
uint32_t mode_of(char const *flags) noexcept
{
    uint32_t mode = 0;
    for (char const *c = flags; *c != '\0'; ++c) {
        switch (*c) {
        case 'r':
        case 'R':
            mode |= 0444;
            break;
        case 'w':
        case 'W':
            mode |= 0222;
            break;
        case 'e':
        case 'E':
            mode |= 0111;
            break;
        case 'd':
        case 'D':
        case 's':
        case 'S':
        case 'p':
        case 'P':
            break; /* no POSIX bit yet (specs/bfs.md decision 7) */
        default:
            break;
        }
    }
    return mode;
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("protect")) {
        std::_Exit(127);
    }
    aegir::args::Result const args =
        aegir::args::read("FILE/A,FLAGS/A", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "protect: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    char const *const file = args.value("FILE");
    char const *const flags = args.value("FLAGS");
    if (::chmod(file, static_cast<mode_t>(mode_of(flags))) != 0) {
        std::fprintf(stderr, "protect: cannot set %s on %s\n", flags, file);
        return 10;
    }
    return 0;
}
