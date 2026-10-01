/*
 * Trinket file-requester path and size helpers.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The parts of the file requester that are pure over the caller's own strings:
 * the drawer above a path, the join of a drawer and a name, and a byte count as
 * the AmigaDOS `List` writes it. Separate from the widget so a host check can
 * pin them without a theme or a VFS (specs/trinket/file_requester.md).
 */

#ifndef AEGIR_TRINKET_FILE_PATH_H
#define AEGIR_TRINKET_FILE_PATH_H

#include <cstdint>
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

/** `bytes` as the AmigaDOS `List` writes it: a comma every three digits. */
std::string format_size(uint64_t bytes);

}  // namespace aegir::trinket::file_path

#endif  // AEGIR_TRINKET_FILE_PATH_H
