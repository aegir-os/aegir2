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

    // Parse a GNU gettext .mo image already in memory. Null when the image is
    // not a .mo (bad magic, a table past the end, and so on). A file and the
    // embedded catalogue both come through here.
    static std::unique_ptr<Translation> parse(std::string_view data);

    // Load a .mo file. `locale_dir` is the LC_MESSAGES directory holding
    // `<domain>.mo` (the system case is deferred; the toolkit embeds).
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

    // Global translation. The static helpers below answer the msgid unchanged
    // when no global is set, so a string is never lost to a missing catalogue.
    static void set_global(std::unique_ptr<Translation> t);
    static const Translation* global();

    static std::string tr(std::string_view msgid);
    static std::string tr(std::string_view msgid, std::string_view msgctxt);
    static std::string ntr(std::string_view msgid, std::string_view msgid_plural, uint64_t n);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Macros for source code (extractable by xgettext): xgettext recognises the
// names TR_, TR_N and TR_C, and the string literal argument is what it takes.
#define TR_(str) aegir::trinket::Translation::tr(str)
#define TR_N(singular, plural, n) aegir::trinket::Translation::ntr(singular, plural, n)
#define TR_C(context, str) aegir::trinket::Translation::tr(str, context)

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_TRANSLATION_H