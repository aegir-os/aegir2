/*
 * Trinket diagnostics: the console cues the test bed reads.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_DIAGNOSTICS_H
#define AEGIR_TRINKET_DIAGNOSTICS_H

#include <aegir/debug.h>
#include <aegir/trinket/widget.h>

namespace aegir::trinket {

/* Report a rectangle as a rect cue: `  rect <name> <x> <y> <width> <height>`,
 * in screen pixels. The acceptance matches the line and clicks the named
 * rectangle instead of a pinned pixel (specs/testing.md's rect cues), so a
 * font or metric change moves the click with the widget. The line is composed
 * from several writes, which is safe because the runtime buffers a line until
 * its newline (aegir-runtime's console_put). The name holds no whitespace;
 * a dotted name namespaces one app's cues from another's. */
inline void report_rect_prefix(char const *prefix, char const *field,
                               Rect const &r)
{
    aegir::debug_write("  rect ");
    if (prefix != nullptr && *prefix != '\0') {
        aegir::debug_write(prefix);
        aegir::debug_write(".");
    }
    aegir::debug_write(field);
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

/* A rectangle with a whole name. */
inline void report_rect(char const *name, Rect const &r)
{
    report_rect_prefix(nullptr, name, r);
}

/* A widget's screen rectangle: what the layouts put it at. */
inline void report_rect(char const *name, Widget const &widget)
{
    report_rect(name, widget.screen_rect());
}

/* The screen rectangle of a rectangle in `widget`'s coordinate space. A
 * widget's internal parts -- a scrollbar's arrow cells, a slider's trough --
 * are in the same space as its `rect()`, so they shift the same way
 * `screen_rect()` shifts it and no parent walk is needed. */
inline Rect screen_rect_of(Widget const &widget, Rect const &part)
{
    Rect const screen = widget.screen_rect();
    Rect const local = widget.rect();
    return {screen.x - local.x + part.x, screen.y - local.y + part.y,
            part.width, part.height};
}

/* A numbered part of a widget: `  rect <prefix>.<part>.<index> <x> <y> <w> <h>`.
 * A tab strip and a radio group report one cue per member this way
 * (specs/testing.md's rect cues). */
inline void report_rect_indexed(char const *prefix, char const *part,
                                int index, Rect const &r)
{
    aegir::debug_write("  rect ");
    aegir::debug_write(prefix);
    aegir::debug_write(".");
    aegir::debug_write(part);
    aegir::debug_write(".");
    aegir::debug_write_unsigned(static_cast<uint64_t>(index));
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

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_DIAGNOSTICS_H
