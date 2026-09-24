/*
 * Trinket Locale implementation.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The locale's data comes from the .locale blob the library embeds
 * (src/locale_data.h, generated from CLDR by scripts/gen_locale_data.py). The
 * format is a flat table of key/value UTF-8 strings behind a small header; a
 * key this file does not know is ignored, so a later CLDR field needs no format
 * change. `Locale()` is the C locale and owns no blob -- its values are the C
 * ones -- which keeps a namespace-scope Locale (the global one) from allocating
 * before the heap is up. Any other name resolves to a shipped blob by its
 * language and falls back to the default locale when there is none
 * (specs/locale.md).
 *
 * The number pattern engine implements the subset CLDR's decimal, percent,
 * currency and scientific formats use: required and optional integer/fraction
 * digits, the primary and secondary group sizes (so a two-group pattern and a
 * three-group one both group right), the `¤`/`%`/`‰` placeholders, and the
 * positive and negative subpatterns. Dates and CLDR plural rules are the next
 * pieces of the arc and are not read yet.
 */

#include <aegir/trinket/locale.h>

#include "locale_data.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aegir::trinket {

namespace {

constexpr char kMagic[] = "AEGLOC1\n";
constexpr std::size_t kMagicLength = 8;
constexpr unsigned int kFlagRtl = 0x0001;

std::uint16_t read_le16(unsigned char const *p)
{
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t read_le32(unsigned char const *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

/* The number symbols a pattern writes with. The defaults are the C locale's. */
struct NumberSymbols {
    std::string_view decimal = ".";
    std::string_view group = ",";
    std::string_view percent = "%";
    std::string_view minus = "-";
    std::string_view plus = "+";
    std::string_view exponential = "E";
    std::string_view permille = "\u2030";
    std::string_view infinity = "\u221e";
    std::string_view nan = "NaN";
    std::string_view digits = "0123456789";
};

/* A parsed number pattern. Its string_views point into the pattern the Impl
 * holds (or a literal), which outlives the format call. */
struct NumberPattern {
    std::string_view prefix;
    std::string_view suffix;
    std::string_view negative_prefix;
    std::string_view negative_suffix;
    bool has_negative = false;
    bool grouping = false;
    bool scientific = false;
    int min_integer = 1;
    int min_fraction = 0;
    int max_fraction = 0;
    int primary_group = 0;
    int secondary_group = 0;
};

bool is_core_char(char c)
{
    return c == '#' || c == '0' || c == ',' || c == '.' || c == 'E';
}

/* One side of a pattern (positive or negative). */
void parse_side(std::string_view side, NumberPattern &out, bool negative)
{
    std::size_t first = std::string_view::npos;
    std::size_t last = std::string_view::npos;
    for (std::size_t i = 0; i < side.size(); ++i) {
        if (is_core_char(side[i])) {
            if (first == std::string_view::npos) {
                first = i;
            }
            last = i;
        }
    }
    if (first == std::string_view::npos) {
        if (negative) {
            out.negative_prefix = side;
        } else {
            out.prefix = side;
        }
        return;
    }
    std::string_view const prefix = side.substr(0, first);
    std::string_view const suffix = side.substr(last + 1);
    if (negative) {
        out.negative_prefix = prefix;
        out.negative_suffix = suffix;
    } else {
        out.prefix = prefix;
        out.suffix = suffix;
    }
    if (negative) {
        return;
    }

    std::string_view const core = side.substr(first, last - first + 1);
    if (core.find('E') != std::string_view::npos) {
        out.scientific = true;
        std::string_view const integer = core.substr(0, core.find('E'));
        out.min_integer = static_cast<int>(std::count(integer.begin(), integer.end(), '0'));
        if (out.min_integer < 1) {
            out.min_integer = 1;
        }
        return;
    }

    std::size_t const point = core.find('.');
    std::string_view const integer = point == std::string_view::npos ? core : core.substr(0, point);
    out.min_integer = static_cast<int>(std::count(integer.begin(), integer.end(), '0'));
    if (out.min_integer < 1) {
        out.min_integer = 1;
    }

    if (integer.find(',') != std::string_view::npos) {
        out.grouping = true;
        std::vector<int> sizes;
        std::size_t start = 0;
        while (start <= integer.size()) {
            std::size_t const comma = integer.find(',', start);
            std::size_t const stop = comma == std::string_view::npos ? integer.size() : comma;
            sizes.push_back(static_cast<int>(stop - start));
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
        out.primary_group = sizes.empty() ? 3 : sizes.back();
        out.secondary_group = sizes.size() >= 3 ? sizes[sizes.size() - 2] : out.primary_group;
        if (out.primary_group < 1) {
            out.primary_group = 3;
        }
        if (out.secondary_group < 1) {
            out.secondary_group = out.primary_group;
        }
    }

    if (point != std::string_view::npos) {
        std::string_view const fraction = core.substr(point + 1);
        out.min_fraction = static_cast<int>(std::count(fraction.begin(), fraction.end(), '0'));
        out.max_fraction =
            out.min_fraction + static_cast<int>(std::count(fraction.begin(), fraction.end(), '#'));
    }
}

NumberPattern parse_pattern(std::string_view pattern)
{
    NumberPattern out;
    std::size_t const semi = pattern.find(';');
    parse_side(pattern.substr(0, semi), out, false);
    if (semi != std::string_view::npos) {
        out.has_negative = true;
        parse_side(pattern.substr(semi + 1), out, true);
    }
    return out;
}

std::vector<std::string> decode_utf8(std::string_view text)
{
    std::vector<std::string> points;
    for (std::size_t i = 0; i < text.size();) {
        unsigned char const c = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if ((c & 0xe0) == 0xc0) {
            length = 2;
        } else if ((c & 0xf0) == 0xe0) {
            length = 3;
        } else if ((c & 0xf8) == 0xf0) {
            length = 4;
        }
        if (i + length > text.size()) {
            length = 1;
        }
        points.emplace_back(text.substr(i, length));
        i += length;
    }
    return points;
}

std::string map_digits(std::string_view ascii, std::string_view digits)
{
    if (digits == "0123456789") {
        return std::string(ascii);
    }
    std::vector<std::string> const mapped = decode_utf8(digits);
    std::string out;
    for (char const c : ascii) {
        if (c >= '0' && c <= '9' && static_cast<std::size_t>(c - '0') < mapped.size()) {
            out += mapped[static_cast<std::size_t>(c - '0')];
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string group_digits(std::string_view integer, int primary, int secondary, std::string_view separator)
{
    std::vector<std::string> groups;
    int end = static_cast<int>(integer.size());
    int size = primary;
    while (end > 0) {
        int const start = std::max(0, end - size);
        groups.emplace_back(integer.substr(static_cast<std::size_t>(start),
                                           static_cast<std::size_t>(end - start)));
        end = start;
        size = secondary;
    }
    std::string out;
    for (std::size_t i = groups.size(); i-- > 0;) {
        if (!out.empty()) {
            out += separator;
        }
        out += groups[i];
    }
    return out;
}

/* Replace the pattern's placeholders -- `¤` currency, `%` percent sign, `‰`
 * permille -- with the locale's symbols. */
std::string expand(std::string_view text, NumberSymbols const &symbols, std::string_view currency)
{
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        unsigned char const c = static_cast<unsigned char>(text[i]);
        if (c == '%') {
            out += symbols.percent;
            i += 1;
        } else if (c == 0xc2 && i + 1 < text.size() &&
                   static_cast<unsigned char>(text[i + 1]) == 0xa4) {
            out += currency;
            i += 2;
        } else if (c == 0xe2 && i + 2 < text.size() &&
                   static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                   static_cast<unsigned char>(text[i + 2]) == 0xb0) {
            out += symbols.permille;
            i += 3;
        } else {
            out.push_back(static_cast<char>(c));
            i += 1;
        }
    }
    return out;
}

std::string body_of(double value, NumberPattern const &pattern, NumberSymbols const &symbols, bool percent)
{
    double v = value < 0.0 ? -value : value;
    if (percent) {
        v *= 100.0;
    }
    int const max_fraction = pattern.max_fraction > 20 ? 20 : pattern.max_fraction;
    char buffer[512];
    std::snprintf(buffer, sizeof buffer, "%.*f", max_fraction, v);
    std::string text = buffer;
    std::string integer;
    std::string fraction;
    std::size_t const point = text.find('.');
    if (point == std::string::npos) {
        integer = text;
    } else {
        integer = text.substr(0, point);
        fraction = text.substr(point + 1);
    }
    while (static_cast<int>(fraction.size()) > pattern.min_fraction && !fraction.empty() &&
           fraction.back() == '0') {
        fraction.pop_back();
    }
    while (static_cast<int>(integer.size()) < pattern.min_integer) {
        integer.insert(integer.begin(), '0');
    }
    std::string result;
    if (pattern.grouping) {
        result = group_digits(integer, pattern.primary_group, pattern.secondary_group, symbols.group);
    } else {
        result = integer;
    }
    result = map_digits(result, symbols.digits);
    if (!fraction.empty()) {
        result += symbols.decimal;
        result += map_digits(fraction, symbols.digits);
    }
    return result;
}

std::string format_fixed(double value, NumberPattern const &pattern, NumberSymbols const &symbols,
                         std::string_view currency, bool percent)
{
    bool const negative = value < 0.0;
    std::string const body = body_of(value, pattern, symbols, percent);
    if (negative && pattern.has_negative) {
        return expand(pattern.negative_prefix, symbols, currency) + body +
               expand(pattern.negative_suffix, symbols, currency);
    }
    std::string const prefix = expand(pattern.prefix, symbols, currency);
    std::string const suffix = expand(pattern.suffix, symbols, currency);
    if (negative) {
        return prefix + std::string(symbols.minus) + body + suffix;
    }
    return prefix + body + suffix;
}

std::string scientific_text(double value, NumberPattern const &pattern, NumberSymbols const &symbols)
{
    bool const negative = value < 0.0;
    double const v = value < 0.0 ? -value : value;
    if (v == 0.0) {
        std::string out = expand(pattern.prefix, symbols, {}) + map_digits("0", symbols.digits) +
                          std::string(symbols.exponential) + map_digits("0", symbols.digits) +
                          expand(pattern.suffix, symbols, {});
        if (negative) {
            out = std::string(symbols.minus) + out;
        }
        return out;
    }
    int exponent = static_cast<int>(std::floor(std::log10(v)));
    double mantissa = v / std::pow(10.0, exponent);
    if (mantissa >= 10.0) {
        mantissa /= 10.0;
        exponent += 1;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.6f", mantissa);
    std::string text = buffer;
    std::string integer;
    std::string fraction;
    std::size_t const point = text.find('.');
    integer = text.substr(0, point);
    fraction = text.substr(point + 1);
    while (!fraction.empty() && fraction.back() == '0') {
        fraction.pop_back();
    }
    while (static_cast<int>(integer.size()) < pattern.min_integer) {
        integer.insert(integer.begin(), '0');
    }
    std::string digits = map_digits(integer, symbols.digits);
    if (!fraction.empty()) {
        digits += symbols.decimal;
        digits += map_digits(fraction, symbols.digits);
    }
    std::string exponent_text;
    if (exponent < 0) {
        exponent_text += symbols.minus;
        exponent = -exponent;
    }
    exponent_text += map_digits(std::to_string(exponent), symbols.digits);
    std::string out = expand(pattern.prefix, symbols, {}) + digits + std::string(symbols.exponential) +
                      exponent_text + expand(pattern.suffix, symbols, {});
    if (negative) {
        out = std::string(symbols.minus) + out;
    }
    return out;
}

std::string substitute_list(std::string_view pattern, std::string_view first, std::string_view second)
{
    std::string out;
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern.compare(i, 3, "{0}") == 0) {
            out += first;
            i += 3;
        } else if (pattern.compare(i, 3, "{1}") == 0) {
            out += second;
            i += 3;
        } else {
            out.push_back(pattern[i]);
            i += 1;
        }
    }
    return out;
}

/* The date names a pattern field writes, all U+001F-joined lists from the
 * blob. Empty for the C locale, whose patterns name nothing. */
struct DateNames {
    std::string_view months_wide;
    std::string_view months_abbreviated;
    std::string_view months_narrow;
    std::string_view days_wide;
    std::string_view days_abbreviated;
    std::string_view days_narrow;
    std::string_view days_short;
    std::string_view periods_wide;
    std::string_view periods_abbreviated;
    std::string_view periods_narrow;
    std::string_view eras_wide;
    std::string_view eras_abbreviated;
    std::string_view eras_narrow;
};

/* A civil (gregorian) date and time, in UTC. */
struct Civil {
    int64_t year;
    int month;   /* 1..12 */
    int day;     /* 1..31 */
    int weekday; /* 0 = Sunday */
    int hour;    /* 0..23 */
    int minute;
    int second;
};

std::string_view field_at(std::string_view joined, int index)
{
    int current = 0;
    std::size_t start = 0;
    while (true) {
        std::size_t const separator = joined.find('\x1f', start);
        std::size_t const stop = separator == std::string_view::npos ? joined.size() : separator;
        if (current == index) {
            return joined.substr(start, stop - start);
        }
        if (separator == std::string_view::npos) {
            return {};
        }
        start = separator + 1;
        current += 1;
    }
}

int64_t floor_div(int64_t dividend, int64_t divisor)
{
    int64_t quotient = dividend / divisor;
    if (dividend % divisor != 0 && (dividend < 0) != (divisor < 0)) {
        quotient -= 1;
    }
    return quotient;
}

/* Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm), so a
 * timestamp is read without musl's C-locale strftime. */
void civil_from_days(int64_t days, int64_t &year, int &month, int &day)
{
    days += 719468;
    int64_t const era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned const day_of_era = static_cast<unsigned>(days - era * 146097);
    unsigned const year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    int64_t const year0 = static_cast<int64_t>(year_of_era) + era * 400;
    unsigned const day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    unsigned const month_prime = (5 * day_of_year + 2) / 153;
    day = static_cast<int>(day_of_year - (153 * month_prime + 2) / 5 + 1);
    month = static_cast<int>(month_prime < 10 ? month_prime + 3 : month_prime - 9);
    year = year0 + (month <= 2 ? 1 : 0);
}

Civil civil_from_timestamp(int64_t seconds)
{
    int64_t const days = floor_div(seconds, 86400);
    int64_t const remainder = seconds - days * 86400;
    Civil civil{};
    civil_from_days(days, civil.year, civil.month, civil.day);
    civil.weekday = static_cast<int>(((days % 7) + 7 + 4) % 7); /* 1970-01-01 was a Thursday */
    civil.hour = static_cast<int>(remainder / 3600);
    civil.minute = static_cast<int>((remainder % 3600) / 60);
    civil.second = static_cast<int>(remainder % 60);
    return civil;
}

std::string pad_number(int64_t value, int width, NumberSymbols const &symbols)
{
    bool const negative = value < 0;
    if (negative) {
        value = -value;
    }
    std::string digits = std::to_string(value);
    while (static_cast<int>(digits.size()) < width) {
        digits.insert(digits.begin(), '0');
    }
    std::string out = map_digits(digits, symbols.digits);
    if (negative) {
        out = std::string(symbols.minus) + out;
    }
    return out;
}

void append_date_field(std::string &out, char letter, int count, Civil const &time, DateNames const &names,
                       NumberSymbols const &symbols)
{
    switch (letter) {
    case 'G': {
        int const index = time.year < 0 ? 0 : 1;
        std::string_view const source =
            count >= 4 ? names.eras_wide : (count == 5 ? names.eras_narrow : names.eras_abbreviated);
        out += field_at(source, index);
        return;
    }
    case 'y': {
        int64_t year = time.year < 0 ? -time.year : time.year;
        if (count == 2) {
            year %= 100;
        }
        out += pad_number(year, count, symbols);
        return;
    }
    case 'M':
    case 'L': {
        if (count >= 3) {
            std::string_view const source = count == 3   ? names.months_abbreviated
                                            : count == 4 ? names.months_wide
                                                         : names.months_narrow;
            out += field_at(source, time.month - 1);
        } else {
            out += pad_number(time.month, count, symbols);
        }
        return;
    }
    case 'd':
        out += pad_number(time.day, count, symbols);
        return;
    case 'E':
    case 'e':
    case 'c': {
        std::string_view const source = count >= 6   ? names.days_short
                                        : count == 4 ? names.days_wide
                                        : count == 5 ? names.days_narrow
                                                     : names.days_abbreviated;
        out += field_at(source, time.weekday);
        return;
    }
    case 'a': {
        std::string_view const source =
            count >= 5 ? names.periods_narrow : (count == 4 ? names.periods_wide : names.periods_abbreviated);
        out += field_at(source, time.hour < 12 ? 0 : 1);
        return;
    }
    case 'h': {
        int hour = time.hour % 12;
        if (hour == 0) {
            hour = 12;
        }
        out += pad_number(hour, count, symbols);
        return;
    }
    case 'H':
        out += pad_number(time.hour, count, symbols);
        return;
    case 'K':
        out += pad_number(time.hour % 12, count, symbols);
        return;
    case 'k': {
        int hour = time.hour;
        if (hour == 0) {
            hour = 24;
        }
        out += pad_number(hour, count, symbols);
        return;
    }
    case 'm':
        out += pad_number(time.minute, count, symbols);
        return;
    case 's':
        out += pad_number(time.second, count, symbols);
        return;
    case 'S':
        /* Sub-second digits: a timestamp is whole seconds, so there are none. */
        return;
    case 'z':
    case 'Z':
    case 'O':
    case 'v':
    case 'V':
    case 'X':
    case 'x':
        out += "UTC"; /* Time zones are deferred; the values are UTC. */
        return;
    default:
        for (int i = 0; i < count; ++i) {
            out.push_back(letter);
        }
        return;
    }
}

std::string format_date_pattern(std::string_view pattern, Civil const &time, DateNames const &names,
                                NumberSymbols const &symbols)
{
    std::string out;
    std::size_t i = 0;
    while (i < pattern.size()) {
        char const c = pattern[i];
        if (c == '\'') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '\'') {
                out.push_back('\'');
                i += 2;
            } else {
                i += 1;
                while (i < pattern.size() && pattern[i] != '\'') {
                    out.push_back(pattern[i]);
                    i += 1;
                }
                if (i < pattern.size()) {
                    i += 1;
                }
            }
            continue;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            std::size_t j = i;
            while (j < pattern.size() && pattern[j] == c) {
                j += 1;
            }
            append_date_field(out, c, static_cast<int>(j - i), time, names, symbols);
            i = j;
            continue;
        }
        out.push_back(c);
        i += 1;
    }
    return out;
}

std::string_view value_or(std::string_view value, std::string_view fallback)
{
    return value.empty() ? fallback : value;
}

/* A number's CLDR plural operands. `plural_form` takes an integer, so the
 * fraction operands and the compact exponent are zero and n and i are it. */
struct PluralOperands {
    long double n;
    long double i;
    long double v;
    long double w;
    long double f;
    long double t;
    long double c;
    long double e;
};

/* A CLDR plural rule: the boolean expression from UTS #35, evaluated against
 * one number's operands. and binds tighter than or; a relation is an operand
 * (optionally mod a value) compared against a comma-separated range list. `=` and
 * `in` are membership; `!=`, `not in`, `not within` negate it. All values are
 * long double, which holds a 64-bit integer exactly on both hosts. */
class PluralRule {
public:
    PluralRule(std::string_view rule, PluralOperands const &operands)
        : text_(rule), position_(0), operands_(operands) {}

    bool matches() { return condition(); }

private:
    std::string_view text_;
    std::size_t position_;
    PluralOperands const &operands_;

    void skip()
    {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t')) {
            position_ += 1;
        }
    }

    bool at_letter(std::size_t position) const
    {
        char const c = text_[position];
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    bool word(std::string_view wanted)
    {
        skip();
        if (text_.compare(position_, wanted.size(), wanted) != 0) {
            return false;
        }
        std::size_t const end = position_ + wanted.size();
        if (end < text_.size() && at_letter(end)) {
            return false;
        }
        position_ = end;
        return true;
    }

    bool punctuation(char c)
    {
        skip();
        if (position_ < text_.size() && text_[position_] == c) {
            position_ += 1;
            return true;
        }
        return false;
    }

    long double number()
    {
        skip();
        long double value = 0.0L;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
            value = value * 10.0L + static_cast<long double>(text_[position_] - '0');
            position_ += 1;
        }
        /* A fraction only if a digit follows the point, so 3..10 does not eat
         * the first dot of the range. */
        if (position_ + 1 < text_.size() && text_[position_] == '.' &&
            text_[position_ + 1] >= '0' && text_[position_ + 1] <= '9') {
            position_ += 1;
            long double scale = 0.1L;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                value += static_cast<long double>(text_[position_] - '0') * scale;
                scale /= 10.0L;
                position_ += 1;
            }
        }
        /* A compact exponent: 1c6 is 1000000. */
        if (position_ + 1 < text_.size() &&
            (text_[position_] == 'c' || text_[position_] == 'e') && text_[position_ + 1] >= '0' &&
            text_[position_ + 1] <= '9') {
            position_ += 1;
            int exponent = 0;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                exponent = exponent * 10 + (text_[position_] - '0');
                position_ += 1;
            }
            for (int i = 0; i < exponent; ++i) {
                value *= 10.0L;
            }
        }
        return value;
    }

    long double operand()
    {
        skip();
        if (position_ >= text_.size()) {
            return 0.0L;
        }
        long double value = 0.0L;
        switch (text_[position_]) {
        case 'n': value = operands_.n; break;
        case 'i': value = operands_.i; break;
        case 'v': value = operands_.v; break;
        case 'w': value = operands_.w; break;
        case 'f': value = operands_.f; break;
        case 't': value = operands_.t; break;
        case 'c': value = operands_.c; break;
        case 'e': value = operands_.e; break;
        default: return 0.0L;
        }
        position_ += 1;
        skip();
        if (word("mod") || punctuation('%')) {
            long double const modulus = number();
            if (modulus != 0.0L) {
                value = std::fmod(value, modulus);
            }
        }
        return value;
    }

    bool range_list(long double value)
    {
        bool found = false;
        do {
            long double const low = number();
            long double high = low;
            skip();
            if (text_.compare(position_, 2, "..") == 0) {
                position_ += 2;
                high = number();
            }
            if (value >= low && value <= high) {
                found = true;
            }
            skip();
        } while (punctuation(','));
        return found;
    }

    bool relation()
    {
        long double const value = operand();
        skip();
        bool negate = false;
        if (punctuation('=')) {
            /* is */
        } else if (punctuation('!')) {
            if (!punctuation('=')) {
                return false;
            }
            negate = true;
        } else if (word("in") || word("within")) {
            /* membership */
        } else if (word("not")) {
            negate = true;
            if (!word("in") && !word("within")) {
                return false;
            }
        } else {
            return false;
        }
        bool const member = range_list(value);
        return negate ? !member : member;
    }

    bool and_condition()
    {
        bool result = relation();
        while (word("and")) {
            bool const right = relation();
            result = result && right;
        }
        return result;
    }

    bool condition()
    {
        bool result = and_condition();
        while (word("or")) {
            bool const right = and_condition();
            result = result || right;
        }
        return result;
    }
};

}  // namespace

struct Locale::Impl {
    std::string name_;
    std::string language_;
    std::string territory_;
    std::string codeset_ = "UTF-8";
    BidiDirection direction_ = BidiDirection::LTR;
    Calendar calendar_ = Calendar::GREGORIAN;
    std::unordered_map<std::string, std::string> properties_;

    std::string_view get(std::string_view key) const
    {
        auto const it = properties_.find(std::string(key));
        return it == properties_.end() ? std::string_view{} : std::string_view(it->second);
    }

    NumberSymbols symbols() const
    {
        NumberSymbols out;
        out.decimal = value_or(get("numbers.symbol.decimal"), out.decimal);
        out.group = value_or(get("numbers.symbol.group"), out.group);
        out.percent = value_or(get("numbers.symbol.percent"), out.percent);
        out.minus = value_or(get("numbers.symbol.minus"), out.minus);
        out.plus = value_or(get("numbers.symbol.plus"), out.plus);
        out.exponential = value_or(get("numbers.symbol.exponential"), out.exponential);
        out.permille = value_or(get("numbers.symbol.permille"), out.permille);
        out.infinity = value_or(get("numbers.symbol.infinity"), out.infinity);
        out.nan = value_or(get("numbers.symbol.nan"), out.nan);
        out.digits = value_or(get("numbers.digits"), out.digits);
        return out;
    }

    std::string_view pattern(std::string_view key, std::string_view fallback) const
    {
        return value_or(get(key), fallback);
    }

    std::string currency_symbol(std::string_view code) const
    {
        if (!code.empty()) {
            std::string_view const symbol = get("currency.symbol." + std::string(code));
            if (!symbol.empty()) {
                return std::string(symbol);
            }
        }
        return std::string(code);
    }

    DateNames date_names() const
    {
        DateNames names;
        names.months_wide = get("months.wide");
        names.months_abbreviated = get("months.abbreviated");
        names.months_narrow = get("months.narrow");
        names.days_wide = get("days.wide");
        names.days_abbreviated = get("days.abbreviated");
        names.days_narrow = get("days.narrow");
        names.days_short = get("days.short");
        names.periods_wide = get("periods.wide");
        names.periods_abbreviated = get("periods.abbreviated");
        names.periods_narrow = get("periods.narrow");
        names.eras_wide = get("eras.wide");
        names.eras_abbreviated = get("eras.abbreviated");
        names.eras_narrow = get("eras.narrow");
        return names;
    }

    bool parse(unsigned char const *data, unsigned int size);
};

bool Locale::Impl::parse(unsigned char const *data, unsigned int size)
{
    if (size < kMagicLength + 8 || std::memcmp(data, kMagic, kMagicLength) != 0) {
        return false;
    }
    std::uint16_t const flags = read_le16(data + 8);
    std::uint32_t const count = read_le32(data + 12);
    std::size_t offset = kMagicLength + 8;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset + 2 > size) {
            return false;
        }
        std::uint16_t const key_length = read_le16(data + offset);
        offset += 2;
        if (offset + key_length > size) {
            return false;
        }
        std::string key(reinterpret_cast<char const *>(data + offset), key_length);
        offset += key_length;
        if (offset + 4 > size) {
            return false;
        }
        std::uint32_t const value_length = read_le32(data + offset);
        offset += 4;
        if (offset + value_length > size) {
            return false;
        }
        std::string value(reinterpret_cast<char const *>(data + offset), value_length);
        offset += value_length;
        properties_[std::move(key)] = std::move(value);
    }
    direction_ = (flags & kFlagRtl) != 0 ? BidiDirection::RTL : BidiDirection::LTR;
    return true;
}

namespace {

detail::LocaleBlob const *find_blob(std::string_view language)
{
    unsigned int count = 0;
    detail::LocaleBlob const *const blobs = detail::locale_blobs(count);
    for (unsigned int i = 0; i < count; ++i) {
        if (language == blobs[i].name) {
            return &blobs[i];
        }
    }
    char const *const fallback = detail::locale_default_name();
    for (unsigned int i = 0; i < count; ++i) {
        if (std::string_view(blobs[i].name) == fallback) {
            return &blobs[i];
        }
    }
    return nullptr;
}

}  // namespace

Locale::Locale() = default;

Locale::Locale(const Locale &other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

Locale &Locale::operator=(const Locale &other)
{
    if (this != &other) {
        impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
    }
    return *this;
}

Locale::~Locale() = default;

Locale::Locale(std::string_view name) : impl_(std::make_unique<Impl>())
{
    std::size_t const underscore = name.find('_');
    std::string_view const language =
        underscore == std::string_view::npos ? name : name.substr(0, underscore);
    std::string_view const territory =
        underscore == std::string_view::npos ? std::string_view{} : name.substr(underscore + 1);
    impl_->name_ = std::string(name);
    impl_->language_ = std::string(language);
    impl_->territory_ = std::string(territory);

    detail::LocaleBlob const *const blob = find_blob(language);
    if (blob != nullptr) {
        static_cast<void>(impl_->parse(blob->data, blob->size));
    }
}

std::string Locale::name() const { return impl_ ? impl_->name_ : "C"; }
std::string Locale::language() const { return impl_ ? impl_->language_ : ""; }
std::string Locale::territory() const { return impl_ ? impl_->territory_ : ""; }
std::string Locale::codeset() const { return impl_ ? impl_->codeset_ : "UTF-8"; }
BidiDirection Locale::direction() const { return impl_ ? impl_->direction_ : BidiDirection::LTR; }
Locale::Calendar Locale::calendar() const
{
    return impl_ ? impl_->calendar_ : Calendar::GREGORIAN;
}

std::string Locale::format_number(double value) const
{
    NumberSymbols const symbols = impl_ ? impl_->symbols() : NumberSymbols{};
    NumberPattern const pattern = parse_pattern(
        impl_ ? impl_->pattern("numbers.decimal", "#,##0.###") : std::string_view("#,##0.###"));
    if (std::isnan(value)) {
        return expand(pattern.prefix, symbols, {}) + std::string(symbols.nan) +
               expand(pattern.suffix, symbols, {});
    }
    if (std::isinf(value)) {
        std::string const prefix = expand(pattern.prefix, symbols, {});
        std::string const info = std::string(symbols.infinity);
        return value < 0.0 ? prefix + std::string(symbols.minus) + info
                           : prefix + info + expand(pattern.suffix, symbols, {});
    }
    return format_fixed(value, pattern, symbols, {}, false);
}

std::string Locale::format_currency(double value, std::string_view currency_code) const
{
    NumberSymbols const symbols = impl_ ? impl_->symbols() : NumberSymbols{};
    NumberPattern const pattern = parse_pattern(
        impl_ ? impl_->pattern("numbers.currency", "\u00a4#,##0.00")
              : std::string_view("\u00a4#,##0.00"));
    std::string const symbol = impl_ ? impl_->currency_symbol(currency_code) : std::string(currency_code);
    return format_fixed(value, pattern, symbols, symbol, false);
}

std::string Locale::format_percent(double value) const
{
    NumberSymbols const symbols = impl_ ? impl_->symbols() : NumberSymbols{};
    NumberPattern const pattern = parse_pattern(
        impl_ ? impl_->pattern("numbers.percent", "#,##0%") : std::string_view("#,##0%"));
    return format_fixed(value, pattern, symbols, {}, true);
}

std::string Locale::format_scientific(double value) const
{
    NumberSymbols const symbols = impl_ ? impl_->symbols() : NumberSymbols{};
    NumberPattern const pattern = parse_pattern(
        impl_ ? impl_->pattern("numbers.scientific", "#E0") : std::string_view("#E0"));
    if (std::isnan(value)) {
        return std::string(symbols.nan);
    }
    if (std::isinf(value)) {
        return value < 0.0 ? std::string(symbols.minus) + std::string(symbols.infinity)
                           : std::string(symbols.infinity);
    }
    return scientific_text(value, pattern, symbols);
}

std::string Locale::format_date(int64_t timestamp, DateFormat fmt) const
{
    static char const *const kKeys[] = {"date.pattern.short", "date.pattern.medium", "date.pattern.long",
                                        "date.pattern.full"};
    std::string_view const iso = "yyyy-MM-dd";
    int index = static_cast<int>(fmt);
    if (index < 0 || index > 3) {
        index = 0;
    }
    std::string_view const pattern = impl_ ? impl_->pattern(kKeys[index], iso) : iso;
    DateNames const names = impl_ ? impl_->date_names() : DateNames{};
    NumberSymbols const symbols = impl_ ? impl_->symbols() : NumberSymbols{};
    return format_date_pattern(pattern, civil_from_timestamp(timestamp), names, symbols);
}

std::string Locale::format_time(int64_t timestamp, TimeFormat fmt) const
{
    static char const *const kKeys[] = {"time.pattern.short", "time.pattern.medium", "time.pattern.long",
                                        "time.pattern.full"};
    std::string_view const iso = "HH:mm:ss";
    int index = static_cast<int>(fmt);
    if (index < 0 || index > 3) {
        index = 0;
    }
    std::string_view const pattern = impl_ ? impl_->pattern(kKeys[index], iso) : iso;
    DateNames const names = impl_ ? impl_->date_names() : DateNames{};
    NumberSymbols const symbols = impl_ ? impl_->symbols() : NumberSymbols{};
    return format_date_pattern(pattern, civil_from_timestamp(timestamp), names, symbols);
}

std::string Locale::format_datetime(int64_t timestamp) const
{
    /* CLDR combines the date and the time with a locale pattern: {1} is the
     * date and {0} the time in the shipped data. */
    std::string_view const combine =
        impl_ ? impl_->pattern("datetime.pattern.short", "{1}, {0}") : std::string_view("{1}, {0}");
    std::string const date = format_date(timestamp, DateFormat::SHORT);
    std::string const time = format_time(timestamp, TimeFormat::SHORT);
    return substitute_list(combine, time, date);
}

Locale::PluralCategory Locale::plural_form(uint64_t n) const
{
    long double const value = static_cast<long double>(n);
    PluralOperands const operands{value, value, 0.0L, 0.0L, 0.0L, 0.0L, 0.0L, 0.0L};
    if (impl_ != nullptr) {
        static constexpr char const *const kCategories[] = {"zero", "one", "two", "few", "many"};
        static constexpr PluralCategory kResults[] = {PluralCategory::ZERO, PluralCategory::ONE,
                                                      PluralCategory::TWO, PluralCategory::FEW,
                                                      PluralCategory::MANY};
        for (std::size_t i = 0; i < 5; ++i) {
            std::string_view const rule = impl_->get(std::string("plural.") + kCategories[i]);
            if (!rule.empty() && PluralRule(rule, operands).matches()) {
                return kResults[i];
            }
        }
    }
    return PluralCategory::OTHER; /* the catch-all, and all the C locale answers */
}

std::string Locale::format_list(std::vector<std::string> const &items) const
{
    if (items.empty()) {
        return "";
    }
    if (items.size() == 1) {
        return items[0];
    }
    if (items.size() == 2) {
        std::string_view const two =
            impl_ ? impl_->pattern("list.two", "{0} and {1}") : std::string_view("{0} and {1}");
        return substitute_list(two, items[0], items[1]);
    }
    std::string_view const end =
        impl_ ? impl_->pattern("list.end", "{0}, and {1}") : std::string_view("{0}, and {1}");
    std::string_view const middle =
        impl_ ? impl_->pattern("list.middle", "{0}, {1}") : std::string_view("{0}, {1}");
    std::string_view const start =
        impl_ ? impl_->pattern("list.start", "{0}, {1}") : std::string_view("{0}, {1}");
    std::string result = items.back();
    result = substitute_list(end, items[items.size() - 2], result);
    for (std::size_t i = items.size() - 2; i-- > 1;) {
        result = substitute_list(middle, items[i], result);
    }
    return substitute_list(start, items[0], result);
}

static std::mutex g_locale_mutex;
static Locale g_global_locale;

void Locale::set_global(const Locale &loc)
{
    std::lock_guard<std::mutex> lock(g_locale_mutex);
    g_global_locale = loc;
}

const Locale &Locale::global() { return g_global_locale; }

std::unique_ptr<Locale> Locale::load(std::string_view path)
{
    /* musl's stdio, not <fstream>: the target links musl's stdio already, and
     * libc++'s file streams would bloat every toolkit image that never calls
     * this. */
    std::FILE *const file = std::fopen(std::string(path).c_str(), "rb");
    if (file == nullptr) {
        return nullptr;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return nullptr;
    }
    long const length = std::ftell(file);
    if (length <= 0) {
        std::fclose(file);
        return nullptr;
    }
    std::rewind(file);
    std::string data(static_cast<std::size_t>(length), '\0');
    std::size_t const got = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (got != data.size()) {
        return nullptr;
    }
    auto locale = std::make_unique<Locale>();
    locale->impl_ = std::make_unique<Impl>();
    if (!locale->impl_->parse(reinterpret_cast<unsigned char const *>(data.data()),
                              static_cast<unsigned int>(data.size()))) {
        return nullptr;
    }
    locale->impl_->name_ = locale->impl_->get("name").empty()
                               ? std::string(path)
                               : std::string(locale->impl_->get("name"));
    locale->impl_->language_ = std::string(locale->impl_->get("language"));
    locale->impl_->territory_ = std::string(locale->impl_->get("territory"));
    return locale;
}

}  // namespace aegir::trinket
