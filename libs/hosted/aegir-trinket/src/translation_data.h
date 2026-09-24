/*
 * The toolkit's embedded gettext catalogue, generated from a .po.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * scripts/compile_po.py emits the implementation into the build tree; it
 * carries the .mo image of resources/translations/trinket.po. The bytes are
 * what Translation::parse reads, so the embedded catalogue and a .mo file on a
 * volume are one format (specs/locale.md). Internal to the toolkit.
 */

#ifndef AEGIR_TRINKET_TRANSLATION_DATA_H
#define AEGIR_TRINKET_TRANSLATION_DATA_H

#include <string_view>

namespace aegir::trinket::detail {

/** The embedded catalogue's .mo image. */
std::string_view translation_blob();

}  // namespace aegir::trinket::detail

#endif  // AEGIR_TRINKET_TRANSLATION_DATA_H
