/*
 * Trinket popup placement (see the header).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/popup.h>

#include <algorithm>

namespace aegir::trinket {

Rect popup_rect(Rect anchor, Size popup, Rect bounds) {
    /* A popup larger than the bounds is the bounds: it cannot be placed inside
     * something smaller than itself, and clipping is the window's. */
    int const width = std::min(popup.width, bounds.width);
    int const height = std::min(popup.height, bounds.height);

    /* Below the widget that opened it; above it when below would leave the
     * bounds; and, when neither fits, at the foot -- the anchor is what the
     * popup belongs to, so it keeps as much of the list visible as it can. */
    int y = anchor.y + anchor.height;
    if (y + height > bounds.y + bounds.height) {
        int const above = anchor.y - height;
        y = above >= bounds.y ? above : bounds.y + bounds.height - height;
    }
    if (y < bounds.y) y = bounds.y;

    /* Its x is the anchor's, pulled left when it would run past the right. */
    int x = anchor.x;
    if (x + width > bounds.x + bounds.width) {
        x = bounds.x + bounds.width - width;
    }
    if (x < bounds.x) x = bounds.x;

    return Rect{x, y, width, height};
}

}  // namespace aegir::trinket
