/*
 * Trinket icons: the MUI artwork a widget names by role.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A list row's image and a file requester's volume rows name an icon, and the
 * theme draws it from the imported MUI XEN art (specs/trinket/listview.md,
 * specs/trinket/file_requester.md). The roles are the artwork's own: the
 * drawer, the hard disk, a floppy, a chip, a volume and a network mount. This
 * header is separate from theme.h so a widget may name an icon without pulling
 * the whole theme in.
 */

#ifndef AEGIR_TRINKET_ICON_H
#define AEGIR_TRINKET_ICON_H

namespace aegir::trinket {

/** The icon a row carries, or NONE for no image. The theme maps each to the
 *  imported MUI artwork (or to nothing); a theme without one draws nothing. */
enum class Icon {
    NONE,
    DRAWER,
    HARD_DISK,
    DISK,
    CHIP,
    VOLUME,
    NETWORK,
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_ICON_H
