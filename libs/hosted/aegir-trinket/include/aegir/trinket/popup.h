/*
 * Trinket popup placement (specs/trinket/popup.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_POPUP_H
#define AEGIR_TRINKET_POPUP_H

#include <aegir/trinket/point.h>

namespace aegir::trinket {

/* Where a popup goes: below the widget that opened it -- `anchor`, in the
 * window's content coordinates -- or above it when `popup` does not fit below,
 * and inside `bounds` either way. Its x is the anchor's, pulled left when it
 * would run past the bounds' right edge. Pure, so `make check-popup` pins it
 * (specs/trinket/popup.md). */
Rect popup_rect(Rect anchor, Size popup, Rect bounds);

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_POPUP_H
