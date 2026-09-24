/*
 * Trinket Locale system.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Binary locale data compiled from CLDR JSON.
 * Supports number/date/currency formatting, plural rules, calendars.
 */

#ifndef AEGIR_TRINKET_LOCALE_H
#define AEGIR_TRINKET_LOCALE_H

#include <aegir/trinket/bidi.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aegir::trinket {

class Locale {
public:
    Locale();                          // "C" locale
    explicit Locale(std::string_view name);  // "en_US", "de_DE", "ja_JP", "ar_SA"

    // The implementation is behind a pointer, so copying is a deep copy rather
    // than the (deleted) pointer copy: `set_global` copies a Locale into the
    // global one.
    Locale(const Locale& other);
    Locale& operator=(const Locale& other);
    Locale(Locale&&) noexcept = default;
    Locale& operator=(Locale&&) noexcept = default;
    ~Locale();

    // Locale identification
    std::string name() const;          // "en_US"
    std::string language() const;      // "en"
    std::string territory() const;     // "US"
    std::string codeset() const;       // "UTF-8"

    // Text direction
    BidiDirection direction() const;

    // Calendar
    enum class Calendar { GREGORIAN, HEBREW, ISLAMIC, JAPANESE, PERSIAN, INDIAN, CHINESE };
    Calendar calendar() const;

    // Number formatting
    std::string format_number(double value) const;
    std::string format_currency(double value, std::string_view currency_code = "USD") const;
    std::string format_percent(double value) const;
    std::string format_scientific(double value) const;

    // Date/time formatting
    enum class DateFormat { SHORT, MEDIUM, LONG, FULL };
    enum class TimeFormat { SHORT, MEDIUM, LONG, FULL };
    std::string format_date(int64_t timestamp, DateFormat fmt = DateFormat::SHORT) const;
    std::string format_time(int64_t timestamp, TimeFormat fmt = TimeFormat::SHORT) const;
    std::string format_datetime(int64_t timestamp) const;

    // Plural forms (CLDR). `other` is the catch-all and the answer of a locale
    // with no rule for the number (the C locale has none).
    enum class PluralCategory { ZERO, ONE, TWO, FEW, MANY, OTHER };
    PluralCategory plural_form(uint64_t n) const;

    // List formatting
    std::string format_list(const std::vector<std::string>& items) const;

    // Global locale
    static void set_global(const Locale& loc);
    static const Locale& global();

    // Load from binary .locale file
    static std::unique_ptr<Locale> load(std::string_view path);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_LOCALE_H