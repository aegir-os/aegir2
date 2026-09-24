/*
 * Host conformance for the toolkit's Locale formatting.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * scripts/check_locale.py compiles this with the host compiler against
 * locale.cc and the generated .locale blobs and runs it. It is not part of any
 * target build. Each check asserts the exact string CLDR's data produces for a
 * shipped locale, so a wrong pattern, symbol, grouping or list join fails.
 * With a path argument it also loads that directory's .locale files through
 * Locale::load, which is the reader the embedded blob and a volume share.
 */

#include <aegir/trinket/locale.h>

#include <cstdio>
#include <memory>
#include <string>
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

}  // namespace

int main(int argc, char **argv)
{
    using aegir::trinket::BidiDirection;
    using aegir::trinket::Locale;

    Locale const en("en");
    expect_string(en.format_number(1234567.891), "1,234,567.891", "en decimal grouping");
    expect_string(en.format_number(0.5), "0.5", "en keeps a short fraction");
    expect_string(en.format_percent(0.25), "25%", "en percent");
    expect_string(en.format_currency(1234.5, "USD"), "$1,234.50", "en currency, prefix");
    expect_string(en.format_currency(1234.5, "EUR"), "\u20ac1,234.50", "en euro symbol");
    expect_string(en.format_currency(5.0, "XTS"), "XTS5.00", "unknown currency falls back to the code");
    expect_string(en.format_scientific(12345.0), "1.2345E4", "en scientific");
    expect_string(en.format_scientific(0.0), "0E0", "en scientific zero");
    expect(en.direction() == BidiDirection::LTR, "en is left-to-right");

    Locale const de("de");
    expect_string(de.format_number(1234567.891), "1.234.567,891", "de separators are swapped");
    expect_string(de.format_currency(1234.5, "EUR"), "1.234,50\u00a0\u20ac", "de currency, suffix");

    Locale const fr("fr");
    expect_string(fr.format_number(1234567.891), "1\u202f234\u202f567,891", "fr narrow no-break group");
    expect_string(fr.format_currency(1234.5, "USD"), "1\u202f234,50\u00a0$US", "fr currency and its own dollar symbol");

    Locale const ru("ru");
    expect_string(ru.format_number(1234567.891), "1\u00a0234\u00a0567,891", "ru no-break group and comma decimal");

    Locale const ar("ar");
    expect(ar.direction() == BidiDirection::RTL, "ar is right-to-left");
    expect_string(ar.format_currency(-5.0, "USD"), "\u200f-5.00\u00a0US$", "ar negative currency subpattern");

    Locale const he("he");
    expect(he.direction() == BidiDirection::RTL, "he is right-to-left");

    Locale const ja("ja");
    expect_string(ja.format_currency(500.0, "JPY"), "\uffe5500.00", "ja yen symbol from its own data");

    expect_string(en.format_list({"a"}), "a", "list of one");
    expect_string(en.format_list({"a", "b"}), "a and b", "list of two");
    expect_string(en.format_list({"a", "b", "c"}), "a, b, and c", "list of three");
    expect_string(en.format_list({"a", "b", "c", "d"}), "a, b, c, and d", "list of four");

    Locale const requested("en_GB");
    expect_string(requested.language(), "en", "language from the name");
    expect_string(requested.territory(), "GB", "territory from the name");
    expect_string(requested.format_currency(1.0, "GBP"), "\u00a31.00", "en_GB resolves to the en blob");

    Locale const unknown("xx");
    expect_string(unknown.format_number(1234.5), "1,234.5", "an unshipped locale falls back to the default");

    Locale const c_locale;
    expect_string(c_locale.name(), "C", "the default locale is the C locale");
    expect_string(c_locale.format_number(1234.5), "1,234.5", "the C locale formats");

    if (argc > 1) {
        std::string const path = std::string(argv[1]) + "/en.locale";
        std::unique_ptr<Locale> const loaded = Locale::load(path);
        expect(loaded != nullptr, "Locale::load reads a .locale file");
        if (loaded != nullptr) {
            expect_string(loaded->format_currency(9.99, "USD"), "$9.99", "a loaded locale formats");
        }
    }

    std::printf("locale: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
