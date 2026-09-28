/*
 * The launcher's window specification, parsed (specs/launch.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A launcher forwards the Amiga `CON:x/y/width/height/title` specification the
 * caller asked for -- a `NEWSHELL WINDOW=...`, eventually a `MULTIVIEW`
 * argument -- and the program that owns the window parses it. The parser lives
 * in the toolkit so every windowed program reads the specification the same
 * way, rather than each carrying its own copy. The options after the title are
 * not read yet.
 */

#ifndef AEGIR_TRINKET_WINDOW_SPEC_H
#define AEGIR_TRINKET_WINDOW_SPEC_H

#include <string>

namespace aegir::trinket {

/** Parse `CON:x/y/width/height/title` into a rectangle and a title. False when
 *  it is malformed, and the caller's default window then stands. */
inline bool parse_window_spec(char const *spec, int *x, int *y, int *width, int *height,
                              std::string *title)
{
    static char const kPrefix[] = "CON:";
    for (unsigned i = 0; i < sizeof(kPrefix) - 1; ++i) {
        if (spec[i] != kPrefix[i]) {
            return false;
        }
    }
    char const *p = spec + sizeof(kPrefix) - 1;
    int *const fields[4] = {x, y, width, height};
    for (int f = 0; f < 4; ++f) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        int value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            ++p;
        }
        *fields[f] = value;
        if (f < 3) {
            if (*p != '/') {
                return false;
            }
            ++p;
        }
    }
    if (*p == '/') {
        ++p;
        while (*p != '\0' && *p != '/') {
            title->push_back(*p);
            ++p;
        }
    }
    return *width > 0 && *height > 0;
}

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_WINDOW_SPEC_H
