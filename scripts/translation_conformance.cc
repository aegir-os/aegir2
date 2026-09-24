/*
 * Host conformance for the toolkit's gettext .mo parser.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * scripts/check_translation.py compiles this with the host compiler against
 * translation.cc and runs it; it is not part of any target build. It builds .mo
 * images in memory -- both byte orders, the header entry, contexts, plurals --
 * and asserts what the parser makes of them, so the format's corners are
 * tested without needing msgfmt or a catalogue on disk.
 */

#include <aegir/trinket/translation.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void expect(bool ok, char const *what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL  %s\n", what);
    }
}

void expect_string(std::string const &got, std::string const &want, char const *what)
{
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::fprintf(stderr, "FAIL  %s: got \"%s\" want \"%s\"\n", what, got.c_str(), want.c_str());
    }
}

void put_u32(std::string &out, std::uint32_t value, bool big)
{
    unsigned char bytes[4];
    if (big) {
        bytes[0] = static_cast<unsigned char>(value >> 24);
        bytes[1] = static_cast<unsigned char>(value >> 16);
        bytes[2] = static_cast<unsigned char>(value >> 8);
        bytes[3] = static_cast<unsigned char>(value);
    } else {
        bytes[0] = static_cast<unsigned char>(value);
        bytes[1] = static_cast<unsigned char>(value >> 8);
        bytes[2] = static_cast<unsigned char>(value >> 16);
        bytes[3] = static_cast<unsigned char>(value >> 24);
    }
    out.append(reinterpret_cast<char const *>(bytes), 4);
}

/* A minimal GNU .mo image: header, the two descriptor tables, the original
 * strings, then the translations. */
std::string build_mo(std::vector<std::pair<std::string, std::string>> const &entries, bool big)
{
    std::uint32_t const count = static_cast<std::uint32_t>(entries.size());
    std::uint32_t const originals_table = 28;
    std::uint32_t const translations_table = originals_table + 8 * count;
    std::uint32_t originals_at = translations_table + 8 * count;
    std::uint32_t translations_at = originals_at;
    for (auto const &entry : entries) {
        translations_at += static_cast<std::uint32_t>(entry.first.size());
    }

    std::vector<std::uint32_t> original_offset(count);
    std::vector<std::uint32_t> translation_offset(count);
    std::uint32_t cursor = originals_at;
    for (std::uint32_t i = 0; i < count; ++i) {
        original_offset[i] = cursor;
        cursor += static_cast<std::uint32_t>(entries[i].first.size());
    }
    cursor = translations_at;
    for (std::uint32_t i = 0; i < count; ++i) {
        translation_offset[i] = cursor;
        cursor += static_cast<std::uint32_t>(entries[i].second.size());
    }

    std::string out;
    put_u32(out, 0x950412de, big); /* the magic, written in the image's order */
    put_u32(out, 0, big);          /* revision */
    put_u32(out, count, big);
    put_u32(out, originals_table, big);
    put_u32(out, translations_table, big);
    put_u32(out, 0, big); /* hash table size */
    put_u32(out, 0, big); /* hash table offset */
    for (std::uint32_t i = 0; i < count; ++i) {
        put_u32(out, static_cast<std::uint32_t>(entries[i].first.size()), big);
        put_u32(out, original_offset[i], big);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        put_u32(out, static_cast<std::uint32_t>(entries[i].second.size()), big);
        put_u32(out, translation_offset[i], big);
    }
    for (auto const &entry : entries) {
        out += entry.first;
    }
    for (auto const &entry : entries) {
        out += entry.second;
    }
    return out;
}

}  // namespace

int main()
{
    using aegir::trinket::Translation;

    /* An embedded NUL cannot come from a string literal: std::string's
     * const char* constructor stops at it. Build the msgids and msgstrs that
     * carry one from their parts. */
    auto context_key = [](std::string_view context, std::string_view msgid) {
        std::string key(context);
        key.push_back('\x04');
        key += msgid;
        return key;
    };
    auto joined = [](std::string_view a, std::string_view b) {
        std::string value(a);
        value.push_back('\0');
        value += b;
        return value;
    };
    auto joined3 = [](std::string_view a, std::string_view b, std::string_view c) {
        std::string value(a);
        value.push_back('\0');
        value += b;
        value.push_back('\0');
        value += c;
        return value;
    };

    std::string const header =
        "Project-Id-Version: trinket-test\n"
        "Content-Type: text/plain; charset=UTF-8\n"
        "Plural-Forms: nplurals=2; plural=(n != 1);\n";

    std::vector<std::pair<std::string, std::string>> const entries = {
        {"", header},
        {"Hello", "Hallo"},
        {context_key("menu", "File"), "Datei"},
        {joined("One file", "%d files"), joined("Eine Datei", "%d Dateien")},
    };

    for (bool const big : {false, true}) {
        std::unique_ptr<Translation> const translation = Translation::parse(build_mo(entries, big));
        expect(translation != nullptr, big ? "a big-endian image parses" : "a little-endian image parses");
        if (translation == nullptr) {
            continue;
        }
        expect_string(translation->translate("Hello"), "Hallo", "a plain msgid translates");
        expect_string(translation->translate("Missing"), "Missing", "an absent msgid is returned");
        expect_string(translation->translate("File", "menu"), "Datei", "a context entry translates");
        expect_string(translation->translate("File", "other"), "File", "an absent context is returned");
        expect(translation->has_translation("Hello"), "has_translation sees a plain entry");
        expect(!translation->has_translation("Missing"), "has_translation sees an absent entry");
        expect_string(translation->ntranslate("One file", "%d files", 1), "Eine Datei",
                      "a plural chooses the singular");
        expect_string(translation->ntranslate("One file", "%d files", 2), "%d Dateien",
                      "a plural chooses the plural");
        expect_string(translation->ntranslate("other", "others", 2), "others",
                      "an absent plural falls back");
    }

    /* A three-form catalogue, to prove the Plural-Forms expression selects the
     * form rather than a singular/plural guess. */
    std::string const three_form_header =
        "Plural-Forms: nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : "
        "n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);\n";
    std::vector<std::pair<std::string, std::string>> const three_form = {
        {"", three_form_header},
        {joined("file", "files"), joined3("f0", "f1", "f2")},
    };
    std::unique_ptr<Translation> const russian = Translation::parse(build_mo(three_form, false));
    expect(russian != nullptr, "a three-form catalogue parses");
    if (russian != nullptr) {
        expect_string(russian->ntranslate("file", "files", 1), "f0", "three forms: 1 is the first");
        expect_string(russian->ntranslate("file", "files", 2), "f1", "three forms: 2 is the second");
        expect_string(russian->ntranslate("file", "files", 5), "f2", "three forms: 5 is the third");
        expect_string(russian->ntranslate("file", "files", 11), "f2", "three forms: 11 is the third");
        expect_string(russian->ntranslate("file", "files", 21), "f0", "three forms: 21 is the first");
    }

    /* Without a Plural-Forms header, the gettext default is n != 1. */
    std::vector<std::pair<std::string, std::string>> const no_header = {
        {joined("file", "files"), joined("one", "many")},
    };
    std::unique_ptr<Translation> const plain = Translation::parse(build_mo(no_header, false));
    expect(plain != nullptr, "a catalogue without a header parses");
    if (plain != nullptr) {
        expect_string(plain->ntranslate("file", "files", 1), "one", "default plural: singular");
        expect_string(plain->ntranslate("file", "files", 7), "many", "default plural: plural");
    }

    expect(Translation::parse("") == nullptr, "an empty image is rejected");
    expect(Translation::parse("not a mo file at all") == nullptr, "a bad magic is rejected");

    /* The built-in catalogue: the committed .po compiled by scripts/compile_po.py
     * and embedded, which is what the demo translates through on target. */
    std::unique_ptr<Translation> const builtin = Translation::embedded();
    expect(builtin != nullptr, "the embedded catalogue parses");
    if (builtin != nullptr) {
        expect_string(builtin->translate("Hello, world"), "Hallo, Welt",
                      "the embedded catalogue translates");
        expect_string(builtin->translate("File", "menu"), "Datei",
                      "the embedded catalogue carries a context");
        expect_string(builtin->ntranslate("%d file", "%d files", 3), "%d Dateien",
                      "the embedded catalogue carries a plural");
    }

    const Translation *const none = Translation::global();
    expect(none == nullptr, "no global translation is set");
    expect_string(Translation::tr("Hello"), "Hello", "tr falls back without a global");
    expect_string(Translation::ntr("one", "many", 3), "many", "ntr falls back without a global");

    std::printf("translation: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
