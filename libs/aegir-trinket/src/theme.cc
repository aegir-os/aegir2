/*
 * Trinket Theme base -- the override hooks' default bodies.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * theme.h declares the drawing primitives as overridable hooks but never
 * defined them, and a class with an out-of-line non-pure virtual needs those
 * bodies to exist: they are the vtable slots, and the class's typeinfo points
 * at them. Nothing needed them while the toolkit built with RTTI off, which is
 * why the gap went unnoticed (specs/cxx.md's exceptions/RTTI arc). The default
 * is to draw nothing -- a theme that wants a primitive overrides it -- and
 * this file existing at all is what emits Theme's key function.
 */

#include <aegir/trinket/theme.h>

#include <aegir/trinket/canvas.h>

namespace aegir::trinket {

void Theme::draw_button(Canvas&, const Rect&, bool, bool, bool, bool, bool) {}

void Theme::draw_panel(Canvas&, const Rect&, Panel::Style, bool) {}

void Theme::draw_textbox(Canvas&, const Rect&, bool, bool, bool) {}

void Theme::draw_menubar(Canvas&, const Rect&) {}

void Theme::draw_menu_item(Canvas&, const Rect&, const char*, bool, bool, bool, bool) {}

void Theme::draw_titlebar(Canvas&, const Rect&, const char*, bool) {}

void Theme::draw_window_frame(Canvas&, const Rect&, bool) {}

void Theme::draw_scrollbar(Canvas&, const Rect&, bool, int, int, bool) {}

void Theme::draw_focus_ring(Canvas&, const Rect&) {}

void Theme::draw_tooltip(Canvas&, const Rect&, const char*) {}

}  // namespace aegir::trinket
