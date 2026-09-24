/*
 * The toolkit's embedded .locale blobs, generated from CLDR.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * scripts/gen_locale_data.py emits the implementation into the build tree; it
 * carries one byte array per locale in manifests/locales.toml and the lookup
 * below. The bytes are the .locale format locale.cc parses, so the embedded
 * data and a .locale file on a volume are the same format (specs/locale.md).
 * Internal to the toolkit.
 */

#ifndef AEGIR_TRINKET_LOCALE_DATA_H
#define AEGIR_TRINKET_LOCALE_DATA_H

namespace aegir::trinket::detail {

struct LocaleBlob {
    char const *name;
    unsigned char const *data;
    unsigned int size;
};

/** The embedded locale blobs, in the manifest's order. */
LocaleBlob const *locale_blobs(unsigned int &count);

/** The locale a name that matches no shipped locale resolves to. */
char const *locale_default_name();

}  // namespace aegir::trinket::detail

#endif  // AEGIR_TRINKET_LOCALE_DATA_H
