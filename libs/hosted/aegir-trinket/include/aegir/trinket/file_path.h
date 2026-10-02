/*
 * Trinket file-requester path helpers.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The parts of the file requester that are pure over the caller's own strings:
 * the drawer above a path and the join of a drawer and a name. Separate from
 * the widget so a host check can pin them without a theme or a VFS
 * (specs/trinket/file_requester.md). A size is not here: it is the locale's
 * (`Locale::format_size`, `specs/locale.md`).
 */

#ifndef AEGIR_TRINKET_FILE_PATH_H
#define AEGIR_TRINKET_FILE_PATH_H

#include <string>
#include <string_view>

namespace aegir::trinket::file_path {

/** The drawer above `path`: the path with its last component dropped. A volume
 *  root ("AEGIR:") is its own parent, as AmigaDOS's is, and an empty path has
 *  none. A trailing separator is dropped first, so "AEGIR:Docs/" is "AEGIR:". */
std::u32string parent_of(std::u32string_view path);

/** `drawer` and `name` joined with the separator the drawer needs: a trailing
 *  colon or slash takes none ("AEGIR:" + "X" is "AEGIR:X"), and anything else
 *  takes a slash. An empty drawer is the name alone. */
std::u32string join(std::u32string_view drawer, std::u32string_view name);

}  // namespace aegir::trinket::file_path

#endif  // AEGIR_TRINKET_FILE_PATH_H
