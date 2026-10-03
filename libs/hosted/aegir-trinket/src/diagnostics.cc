/*
 * Trinket diagnostics implementation (see the header).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/diagnostics.h>

#include <map>
#include <string>

namespace aegir::trinket {

namespace {

/* The last rectangle reported for each name. A report runs on every poll, and
 * each cue is a synchronous logger call; without this a widget that did not
 * move rewrote its line every poll, and the flood made the guest crawl (the
 * boot took minutes longer while the logger drained it). A change is what the
 * acceptance needs to hear, so a change is what is written. */
std::map<std::string, Rect>& last_rects()
{
    static std::map<std::string, Rect> rects;
    return rects;
}

bool same(Rect const &a, Rect const &b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width &&
           a.height == b.height;
}

void write_rect(Rect const &r)
{
    aegir::debug_write(" ");
    aegir::debug_write_unsigned(static_cast<uint64_t>(r.x));
    aegir::debug_write(" ");
    aegir::debug_write_unsigned(static_cast<uint64_t>(r.y));
    aegir::debug_write(" ");
    aegir::debug_write_unsigned(static_cast<uint64_t>(r.width));
    aegir::debug_write(" ");
    aegir::debug_write_unsigned(static_cast<uint64_t>(r.height));
    aegir::debug_write("\n");
}

void report(std::string const &name, Rect const &r)
{
    std::map<std::string, Rect>& rects = last_rects();
    auto it = rects.find(name);
    if (it != rects.end() && same(it->second, r)) {
        return;
    }
    rects[name] = r;
    aegir::debug_write("  rect ");
    aegir::debug_write(name.c_str());
    write_rect(r);
}

}  // namespace

void report_rect_prefix(char const *prefix, char const *field, Rect const &r)
{
    std::string name;
    if (prefix != nullptr && *prefix != '\0') {
        name = prefix;
        name += '.';
    }
    name += field;
    report(name, r);
}

void report_rect_indexed(char const *prefix, char const *part, int index,
                         Rect const &r)
{
    std::string name(prefix);
    name += '.';
    name += part;
    name += '.';
    name += std::to_string(index);
    report(name, r);
}

}  // namespace aegir::trinket
