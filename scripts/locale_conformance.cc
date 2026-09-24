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

    Locale const zh("zh");

    expect_string(en.format_list({"a"}), "a", "list of one");
    expect_string(en.format_list({"a", "b"}), "a and b", "list of two");
    expect_string(en.format_list({"a", "b", "c"}), "a, b, and c", "list of three");
    expect_string(en.format_list({"a", "b", "c", "d"}), "a, b, c, and d", "list of four");

    Locale const requested("en_GB");
    expect_string(requested.language(), "en", "language from the name");
    expect_string(requested.territory(), "GB", "territory from the name");
    expect_string(requested.format_currency(1.0, "GBP"), "\u00a31.00", "en_GB resolves to the en blob");

    /* Dates and times from the CLDR gregorian calendar, in UTC. The epoch is
     * 1970-01-01, a Thursday; 1614959229 is 2021-03-05, a Friday. */
    expect_string(en.format_date(0, Locale::DateFormat::SHORT), "1/1/70", "en date, short");
    expect_string(en.format_date(0, Locale::DateFormat::MEDIUM), "Jan 1, 1970", "en date, medium");
    expect_string(en.format_date(0, Locale::DateFormat::LONG), "January 1, 1970", "en date, long");
    expect_string(en.format_date(0, Locale::DateFormat::FULL), "Thursday, January 1, 1970",
                  "en date, full names a weekday");
    expect_string(en.format_time(0, Locale::TimeFormat::SHORT), "12:00\u202fAM", "en time, short");
    expect_string(en.format_time(0, Locale::TimeFormat::MEDIUM), "12:00:00\u202fAM", "en time, medium");
    expect_string(en.format_datetime(0), "1/1/70, 12:00\u202fAM", "en date and time combined");
    expect_string(en.format_date(1614959229, Locale::DateFormat::FULL), "Friday, March 5, 2021",
                  "en resolves a later date");
    expect_string(en.format_time(1614959229, Locale::TimeFormat::MEDIUM), "3:47:09\u202fPM",
                  "en resolves a later time");

    expect_string(de.format_date(0, Locale::DateFormat::MEDIUM), "01.01.1970", "de date, day first");
    expect_string(de.format_time(0, Locale::TimeFormat::SHORT), "00:00", "de time is 24-hour");
    expect_string(fr.format_date(0, Locale::DateFormat::MEDIUM), "1 janv. 1970", "fr month name");
    expect_string(fr.format_datetime(0), "01/01/1970 00:00", "fr combines without a comma");
    expect_string(ru.format_date(0, Locale::DateFormat::MEDIUM), "1 \u044f\u043d\u0432. 1970\u202f\u0433.",
                  "ru date with its quoted literal");
    expect_string(ar.format_date(0, Locale::DateFormat::SHORT), "1\u200f/1\u200f/1970",
                  "ar date with its direction marks");
    expect_string(ar.format_time(0, Locale::TimeFormat::SHORT), "12:00 \u0635", "ar day period");
    expect_string(ar.format_datetime(0), "1\u200f/1\u200f/1970\u060c 12:00 \u0635",
                  "ar combines with an Arabic comma");
    expect_string(ja.format_date(0, Locale::DateFormat::SHORT), "1970/01/01", "ja date, zero padded");
    expect_string(zh.format_date(0, Locale::DateFormat::MEDIUM), "1970\u5e741\u67081\u65e5",
                  "zh date with its markers");

    Locale const unknown("xx");
    expect_string(unknown.format_number(1234.5), "1,234.5", "an unshipped locale falls back to the default");

    /* The CLDR plural rules, evaluated against an integer. */
    using PluralCategory = Locale::PluralCategory;
    Locale const es("es");
    expect(en.plural_form(1) == PluralCategory::ONE, "en plural: one");
    expect(en.plural_form(2) == PluralCategory::OTHER, "en plural: other");
    expect(de.plural_form(1) == PluralCategory::ONE, "de plural: one");
    expect(es.plural_form(1000000) == PluralCategory::MANY, "es plural: many from a million");
    expect(ja.plural_form(1) == PluralCategory::OTHER, "ja has only other");
    expect(zh.plural_form(1) == PluralCategory::OTHER, "zh has only other");
    expect(ar.plural_form(0) == PluralCategory::ZERO, "ar plural: zero");
    expect(ar.plural_form(1) == PluralCategory::ONE, "ar plural: one");
    expect(ar.plural_form(2) == PluralCategory::TWO, "ar plural: two");
    expect(ar.plural_form(3) == PluralCategory::FEW, "ar plural: few");
    expect(ar.plural_form(11) == PluralCategory::MANY, "ar plural: many");
    expect(ar.plural_form(100) == PluralCategory::OTHER, "ar plural: other");
    expect(ru.plural_form(1) == PluralCategory::ONE, "ru plural: one");
    expect(ru.plural_form(2) == PluralCategory::FEW, "ru plural: few");
    expect(ru.plural_form(5) == PluralCategory::MANY, "ru plural: many");
    expect(ru.plural_form(21) == PluralCategory::ONE, "ru plural: one past twenty");
    expect(ru.plural_form(22) == PluralCategory::FEW, "ru plural: few past twenty");
    expect(ru.plural_form(12) == PluralCategory::MANY, "ru plural: the teen exception");
    expect(fr.plural_form(0) == PluralCategory::ONE, "fr plural: zero is one");
    expect(fr.plural_form(1) == PluralCategory::ONE, "fr plural: one");
    expect(fr.plural_form(2) == PluralCategory::OTHER, "fr plural: other");
    expect(fr.plural_form(1000000) == PluralCategory::MANY, "fr plural: many from a million");
    expect(he.plural_form(1) == PluralCategory::ONE, "he plural: one");
    expect(he.plural_form(2) == PluralCategory::TWO, "he plural: two");
    expect(he.plural_form(3) == PluralCategory::OTHER, "he plural: other");
    expect(Locale().plural_form(1) == PluralCategory::OTHER, "the C locale has no plural rules");

    Locale const c_locale;
    expect_string(c_locale.name(), "C", "the default locale is the C locale");
    expect_string(c_locale.format_number(1234.5), "1,234.5", "the C locale formats");
    expect_string(c_locale.format_date(0), "1970-01-01", "the C locale dates in ISO");
    expect_string(c_locale.format_time(0), "00:00:00", "the C locale times in ISO");

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
