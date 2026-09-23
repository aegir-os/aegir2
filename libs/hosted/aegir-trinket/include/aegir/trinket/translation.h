/*
 * Trinket Translation system.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * GNU gettext .mo binary format parser.
 * Compatible with xgettext extraction.
 */

#ifndef AEGIR_TRINKET_TRANSLATION_H
#define AEGIR_TRINKET_TRANSLATION_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aegir::trinket {

class Translation {
public:
    Translation() = default;
    ~Translation();

    // Load .mo file (GNU gettext binary format)
    static std::unique_ptr<Translation> load(std::string_view domain,
                                              std::string_view locale_dir);
    static std::unique_ptr<Translation> load_from_file(std::string_view path);

    // Core translation functions
    std::string translate(std::string_view msgid) const;
    std::string translate(std::string_view msgid, std::string_view msgctxt) const;
    std::string ntranslate(std::string_view msgid, std::string_view msgid_plural,
                           uint64_t n) const;

    // For widget use
    std::string operator()(std::string_view msgid) const { return translate(msgid); }

    // Check if translation exists
    bool has_translation(std::string_view msgid) const;

    // Global translation
    static void set_global(std::unique_ptr<Translation> t);
    static const Translation* global();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Macros for source code (extractable by xgettext)
#define TR_(str) aegir::trinket::Translation::global()->translate(str)
#define TR_N(singular, plural, n) aegir::trinket::Translation::global()->ntranslate(singular, plural, n)
#define TR_C(context, str) aegir::trinket::Translation::global()->translate(str, context)

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_TRANSLATION_H