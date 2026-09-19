/*
 * Trinket Locale implementation.
 */

#include <aegir/trinket/locale.h>
#include <aegir/trinket/unicode.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace aegir::trinket {

struct Locale::Impl {
    std::string name_;
    std::string language_;
    std::string territory_;
    std::string codeset_ = "UTF-8";
    BidiDirection direction_ = BidiDirection::LTR;
    Calendar calendar_ = Calendar::GREGORIAN;

    // Number formatting
    std::string decimal_sep_ = ".";
    std::string group_sep_ = ",";
    std::string percent_sign_ = "%";
    std::string currency_symbol_ = "$";
    std::string currency_format_ = "¤#,##0.00";

    // Date/time
    std::string date_short_ = "MM/dd/yyyy";
    std::string date_long_ = "MMMM d, yyyy";
    std::string time_short_ = "HH:mm";
    std::string time_long_ = "HH:mm:ss z";

    // Plural rules (simplified - real impl would parse CLDR)
    std::string plural_rule_ = "n != 1";  // English-like
};

Locale::Locale() = default;

Locale::Locale(std::string_view name) : impl_(std::make_unique<Impl>()) {
    impl_->name_ = name;
    size_t underscore = name.find('_');
    if (underscore != std::string::npos) {
        impl_->language_ = name.substr(0, underscore);
        impl_->territory_ = name.substr(underscore + 1);
    } else {
        impl_->language_ = name;
    }

    // Set defaults based on language
    if (impl_->language_ == "de") {
        impl_->decimal_sep_ = ",";
        impl_->group_sep_ = ".";
        impl_->date_short_ = "dd.MM.yyyy";
        impl_->time_short_ = "HH:mm";
        impl_->currency_symbol_ = "€";
        impl_->currency_format_ = "#,##0.00 ¤";
    } else if (impl_->language_ == "fr") {
        impl_->decimal_sep_ = ",";
        impl_->group_sep_ = " ";
        impl_->date_short_ = "dd/MM/yyyy";
        impl_->currency_symbol_ = "€";
        impl_->currency_format_ = "#,##0.00 ¤";
    } else if (impl_->language_ == "ja") {
        impl_->date_short_ = "yyyy/MM/dd";
        impl_->time_short_ = "HH:mm";
        impl_->currency_symbol_ = "¥";
        impl_->currency_format_ = "¤#,##0";
    } else if (impl_->language_ == "ar") {
        impl_->direction_ = BidiDirection::RTL;
        impl_->decimal_sep_ = "،";
        impl_->group_sep_ = "٬";
        impl_->percent_sign_ = "%";
        impl_->calendar_ = Calendar::ISLAMIC;
        impl_->date_short_ = "dd/MM/yyyy";
        impl_->currency_symbol_ = "﷼";
        impl_->currency_format_ = "¤#,##0.00";
    } else if (impl_->language_ == "he") {
        impl_->direction_ = BidiDirection::RTL;
        impl_->calendar_ = Calendar::HEBREW;
        impl_->date_short_ = "dd/MM/yyyy";
        impl_->currency_symbol_ = "₪";
        impl_->currency_format_ = "¤#,##0.00";
    }
}

std::string Locale::name() const { return impl_ ? impl_->name_ : "C"; }
std::string Locale::language() const { return impl_ ? impl_->language_ : "en"; }
std::string Locale::territory() const { return impl_ ? impl_->territory_ : "US"; }
std::string Locale::codeset() const { return impl_ ? impl_->codeset_ : "UTF-8"; }
BidiDirection Locale::direction() const { return impl_ ? impl_->direction_ : BidiDirection::LTR; }
Locale::Calendar Locale::calendar() const { return impl_ ? impl_->calendar_ : Calendar::GREGORIAN; }

std::string Locale::format_number(double value) const {
    if (!impl_) return std::to_string(value);
    char buf[128];
    // Simplified formatting
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    std::string s = buf;
    // Replace decimal point
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        s.replace(dot, 1, impl_->decimal_sep_);
    }
    return s;
}

std::string Locale::format_currency(double value, std::string_view currency_code) const {
    if (!impl_) return std::to_string(value);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    std::string s = buf;
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        s.replace(dot, 1, impl_->decimal_sep_);
    }
    return impl_->currency_symbol_ + s;
}

std::string Locale::format_percent(double value) const {
    if (!impl_) return std::to_string(value) + "%";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.2f", value * 100);
    std::string s = buf;
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        s.replace(dot, 1, impl_->decimal_sep_);
    }
    return s + impl_->percent_sign_;
}

std::string Locale::format_scientific(double value) const {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%e", value);
    return buf;
}

std::string Locale::format_date(int64_t timestamp, DateFormat fmt) const {
    if (!impl_) return "";
    std::time_t t = static_cast<std::time_t>(timestamp);
    std::tm tm = *std::localtime(&t);
    const char* pattern = "";
    switch (fmt) {
        case DateFormat::SHORT: pattern = impl_->date_short_.c_str(); break;
        case DateFormat::MEDIUM: pattern = impl_->date_long_.c_str(); break;
        default: pattern = impl_->date_long_.c_str(); break;
    }
    // Simplified - real impl would parse pattern
    char buf[128];
    std::strftime(buf, sizeof(buf), "%x", &tm);
    return buf;
}

std::string Locale::format_time(int64_t timestamp, TimeFormat fmt) const {
    if (!impl_) return "";
    std::time_t t = static_cast<std::time_t>(timestamp);
    std::tm tm = *std::localtime(&t);
    const char* pattern = "";
    switch (fmt) {
        case TimeFormat::SHORT: pattern = impl_->time_short_.c_str(); break;
        case TimeFormat::MEDIUM: pattern = impl_->time_long_.c_str(); break;
        default: pattern = impl_->time_long_.c_str(); break;
    }
    char buf[128];
    std::strftime(buf, sizeof(buf), "%X", &tm);
    return buf;
}

std::string Locale::format_datetime(int64_t timestamp) const {
    return format_date(timestamp) + " " + format_time(timestamp);
}

int Locale::plural_form(uint64_t n) const {
    if (!impl_) return n == 1 ? 0 : 1;
    // Simplified: English-like
    if (impl_->language_ == "ar") {
        // Arabic has 6 plural forms
        if (n == 0) return 0;
        if (n == 1) return 1;
        if (n == 2) return 2;
        if (n % 100 >= 3 && n % 100 <= 10) return 3;
        if (n % 100 >= 11) return 4;
        return 5;
    }
    return n == 1 ? 0 : 1;
}

std::string Locale::format_list(const std::vector<std::string>& items) const {
    if (items.empty()) return "";
    if (items.size() == 1) return items[0];
    if (items.size() == 2) return items[0] + " and " + items[1];
    std::string result;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) result += ", ";
        if (i == items.size() - 1) result += "and ";
        result += items[i];
    }
    return result;
}

static std::mutex g_locale_mutex;
static Locale g_global_locale;

void Locale::set_global(const Locale& loc) {
    std::lock_guard<std::mutex> lock(g_locale_mutex);
    g_global_locale = loc;
}

const Locale& Locale::global() {
    return g_global_locale;
}

std::unique_ptr<Locale> Locale::load(std::string_view path) {
    // TODO: Load from binary .locale file
    return nullptr;
}

} // namespace aegir::trinket