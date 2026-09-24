/*
 * Trinket Translation implementation.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The GNU gettext .mo format, read from a buffer so a file and an embedded
 * catalogue come through one path. The image is a header of seven 32-bit words
 * and two parallel tables of (length, offset) descriptors for the original
 * strings and their translations. The words are in the image's own byte order,
 * which the magic number names; a .mo is asked to carry it so it can be read
 * on either endianness.
 *
 * Three kinds of entry share the table, told apart by the msgid's bytes: the
 * empty msgid is the header (whose msgstr carries Plural-Forms); a msgctxt
 * entry separates the context and the msgid with EOT (U+0004); a plural entry
 * separates the singular and the plural with NUL, and its msgstr holds one
 * form per NUL. The header's Plural-Forms gives `nplurals` and a C expression
 * in n that selects the form, which ntranslate evaluates.
 *
 * `load`/`load_from_file` read a file through musl's stdio (libc++ file
 * streams would bloat every toolkit image); reading a catalogue from the
 * system is deferred, so the toolkit embeds one.
 */

#include <aegir/trinket/translation.h>

#include "translation_data.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aegir::trinket {

namespace {

constexpr std::uint32_t kMagicLittle = 0x950412de;
constexpr std::uint32_t kMagicBig = 0xde120495;
constexpr char kContextSeparator = '\x04'; /* EOT, as gettext defines it */

std::uint32_t read_u32(unsigned char const *data, bool big)
{
    if (big) {
        return (static_cast<std::uint32_t>(data[0]) << 24) |
               (static_cast<std::uint32_t>(data[1]) << 16) |
               (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
    }
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint32_t read_u32_at(std::string_view data, std::size_t offset, bool big)
{
    return read_u32(reinterpret_cast<unsigned char const *>(data.data()) + offset, big);
}

std::string_view slice(std::string_view data, std::uint32_t offset, std::uint32_t length)
{
    return data.substr(offset, length);
}

/* The index-th NUL-separated field of a plural msgstr. */
std::string_view nul_field(std::string_view blob, int index)
{
    int current = 0;
    std::size_t start = 0;
    while (true) {
        std::size_t const separator = blob.find('\0', start);
        std::size_t const stop = separator == std::string_view::npos ? blob.size() : separator;
        if (current == index) {
            return blob.substr(start, stop - start);
        }
        if (separator == std::string_view::npos) {
            return {};
        }
        start = separator + 1;
        current += 1;
    }
}

/* The gettext Plural-Forms expression: a C expression in n with the usual
 * precedence, evaluated to the msgstr index. An expression that does not parse
 * leaves everything at zero, and ntranslate's fallback takes over. */
struct PluralEval {
    std::string_view text;
    std::size_t position = 0;
    std::int64_t n = 0;

    void skip()
    {
        while (position < text.size() && (text[position] == ' ' || text[position] == '\t')) {
            position += 1;
        }
    }

    bool take(std::string_view token)
    {
        skip();
        if (text.compare(position, token.size(), token) != 0) {
            return false;
        }
        position += token.size();
        return true;
    }

    bool take_char(char c)
    {
        skip();
        if (position < text.size() && text[position] == c) {
            position += 1;
            return true;
        }
        return false;
    }

    bool identifier_end(std::size_t at) const
    {
        if (at >= text.size()) {
            return true;
        }
        char const c = text[at];
        return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
    }

    std::int64_t run() { return conditional(); }

    std::int64_t conditional()
    {
        std::int64_t const condition = logical_or();
        if (take_char('?')) {
            std::int64_t const yes = conditional();
            if (!take_char(':')) {
                return yes;
            }
            std::int64_t const no = conditional();
            return condition != 0 ? yes : no;
        }
        return condition;
    }

    std::int64_t logical_or()
    {
        std::int64_t value = logical_and();
        while (take("||")) {
            /* Parse the right operand always: && and || here must not
             * short-circuit, or the scan would stop mid-expression. */
            std::int64_t const right = logical_and();
            value = (value != 0 || right != 0) ? 1 : 0;
        }
        return value;
    }

    std::int64_t logical_and()
    {
        std::int64_t value = equality();
        while (take("&&")) {
            std::int64_t const right = equality();
            value = (value != 0 && right != 0) ? 1 : 0;
        }
        return value;
    }

    std::int64_t equality()
    {
        std::int64_t value = relational();
        while (true) {
            if (take("==")) {
                value = value == relational() ? 1 : 0;
            } else if (take("!=")) {
                value = value != relational() ? 1 : 0;
            } else {
                return value;
            }
        }
    }

    std::int64_t relational()
    {
        std::int64_t value = additive();
        while (true) {
            if (take("<=")) {
                value = value <= additive() ? 1 : 0;
            } else if (take(">=")) {
                value = value >= additive() ? 1 : 0;
            } else if (take("<")) {
                value = value < additive() ? 1 : 0;
            } else if (take(">")) {
                value = value > additive() ? 1 : 0;
            } else {
                return value;
            }
        }
    }

    std::int64_t additive()
    {
        std::int64_t value = multiplicative();
        while (true) {
            if (take_char('+')) {
                value += multiplicative();
            } else if (take_char('-')) {
                value -= multiplicative();
            } else {
                return value;
            }
        }
    }

    std::int64_t multiplicative()
    {
        std::int64_t value = unary();
        while (true) {
            if (take_char('*')) {
                value *= unary();
            } else if (take_char('/')) {
                std::int64_t const divisor = unary();
                value = divisor == 0 ? 0 : value / divisor;
            } else if (take_char('%')) {
                std::int64_t const divisor = unary();
                value = divisor == 0 ? 0 : value % divisor;
            } else {
                return value;
            }
        }
    }

    std::int64_t unary()
    {
        if (take_char('!')) {
            return unary() == 0 ? 1 : 0;
        }
        if (take_char('-')) {
            return -unary();
        }
        if (take_char('+')) {
            return unary();
        }
        return primary();
    }

    std::int64_t primary()
    {
        skip();
        if (take_char('(')) {
            std::int64_t const value = conditional();
            take_char(')');
            return value;
        }
        if (position < text.size() && text[position] == 'n' && identifier_end(position + 1)) {
            position += 1;
            return n;
        }
        std::int64_t value = 0;
        bool any = false;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            value = value * 10 + (text[position] - '0');
            position += 1;
            any = true;
        }
        (void)any;
        return value;
    }
};

int evaluate_plural(std::string_view expression, std::int64_t n)
{
    if (expression.empty()) {
        return n == 1 ? 0 : 1;
    }
    PluralEval eval{expression, 0, n};
    return static_cast<int>(eval.run());
}

}  // namespace

struct Translation::Impl {
    std::unordered_map<std::string, std::string> singular_;
    /* msgid -> the NUL-joined msgstr forms, for the entries whose msgid has a
     * NUL (a singular and a plural). */
    std::unordered_map<std::string, std::string> plural_;
    /* context -> msgid -> msgstr; a plural with a context keeps its NUL-joined
     * forms here too, though only the singular call is exposed. */
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> context_;
    int nplurals_ = 2;
    std::string plural_forms_ = "(n != 1)";

    void parse_header(std::string_view header)
    {
        std::size_t start = 0;
        while (start < header.size()) {
            std::size_t const newline = header.find('\n', start);
            std::string_view line =
                header.substr(start, newline == std::string_view::npos ? std::string_view::npos
                                                                       : newline - start);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            std::string_view const key = "Plural-Forms:";
            if (line.substr(0, key.size()) == key) {
                std::string_view value = line.substr(key.size());
                std::size_t const nplurals = value.find("nplurals=");
                if (nplurals != std::string_view::npos) {
                    int count = 0;
                    std::size_t at = nplurals + 9;
                    while (at < value.size() && value[at] >= '0' && value[at] <= '9') {
                        count = count * 10 + (value[at] - '0');
                        at += 1;
                    }
                    if (count > 0) {
                        nplurals_ = count;
                    }
                }
                std::size_t const plural = value.find("plural=");
                if (plural != std::string_view::npos) {
                    std::string_view expression = value.substr(plural + 7);
                    while (!expression.empty() &&
                           (expression.back() == ';' || expression.back() == ' ' ||
                            expression.back() == '\t')) {
                        expression.remove_suffix(1);
                    }
                    plural_forms_ = std::string(expression);
                }
            }
            if (newline == std::string_view::npos) {
                break;
            }
            start = newline + 1;
        }
    }
};

Translation::~Translation() = default;

std::unique_ptr<Translation> Translation::parse(std::string_view data)
{
    if (data.size() < 28) {
        return nullptr;
    }
    unsigned char const *const bytes = reinterpret_cast<unsigned char const *>(data.data());
    std::uint32_t const magic = read_u32(bytes, false);
    bool big = false;
    if (magic == kMagicLittle) {
        big = false;
    } else if (magic == kMagicBig) {
        big = true;
    } else {
        return nullptr;
    }

    std::uint32_t const count = read_u32(bytes + 8, big);
    std::uint32_t const originals = read_u32(bytes + 12, big);
    std::uint32_t const translations = read_u32(bytes + 16, big);

    /* Both descriptor tables must lie inside the image, and the descriptors
     * they name must too, or the image is not a usable .mo. */
    std::uint64_t const tables_bytes = static_cast<std::uint64_t>(count) * 8;
    if (static_cast<std::uint64_t>(originals) + tables_bytes > data.size() ||
        static_cast<std::uint64_t>(translations) + tables_bytes > data.size()) {
        return nullptr;
    }

    auto translation = std::make_unique<Translation>();
    translation->impl_ = std::make_unique<Impl>();

    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t const original_length = read_u32_at(data, originals + i * 8, big);
        std::uint32_t const original_offset = read_u32_at(data, originals + i * 8 + 4, big);
        std::uint32_t const translated_length = read_u32_at(data, translations + i * 8, big);
        std::uint32_t const translated_offset = read_u32_at(data, translations + i * 8 + 4, big);
        if (static_cast<std::uint64_t>(original_offset) + original_length > data.size() ||
            static_cast<std::uint64_t>(translated_offset) + translated_length > data.size()) {
            return nullptr;
        }
        std::string_view const original = slice(data, original_offset, original_length);
        std::string_view const translated = slice(data, translated_offset, translated_length);

        if (original_length == 0) {
            translation->impl_->parse_header(translated);
            continue;
        }
        std::size_t const context = original.find(kContextSeparator);
        if (context != std::string_view::npos) {
            std::string_view const key = original.substr(0, context);
            translation->impl_->context_[std::string(key)][std::string(original.substr(context + 1))] =
                std::string(translated);
            continue;
        }
        std::size_t const plural = original.find('\0');
        if (plural != std::string_view::npos) {
            translation->impl_->plural_[std::string(original.substr(0, plural))] =
                std::string(translated);
            continue;
        }
        translation->impl_->singular_[std::string(original)] = std::string(translated);
    }

    return translation;
}

std::unique_ptr<Translation> Translation::embedded()
{
    return parse(detail::translation_blob());
}

std::unique_ptr<Translation> Translation::load(std::string_view domain, std::string_view locale_dir)
{
    std::string path(locale_dir);
    path += '/';
    path += domain;
    path += ".mo";
    return load_from_file(path);
}

std::unique_ptr<Translation> Translation::load_from_file(std::string_view path)
{
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
    return parse(data);
}

std::string Translation::translate(std::string_view msgid) const
{
    if (impl_ != nullptr) {
        auto const it = impl_->singular_.find(std::string(msgid));
        if (it != impl_->singular_.end()) {
            return it->second;
        }
    }
    return std::string(msgid);
}

std::string Translation::translate(std::string_view msgid, std::string_view msgctxt) const
{
    if (impl_ != nullptr) {
        auto const context = impl_->context_.find(std::string(msgctxt));
        if (context != impl_->context_.end()) {
            auto const it = context->second.find(std::string(msgid));
            if (it != context->second.end()) {
                return it->second;
            }
        }
    }
    return std::string(msgid);
}

std::string Translation::ntranslate(std::string_view msgid, std::string_view msgid_plural,
                                    uint64_t n) const
{
    if (impl_ != nullptr) {
        auto const it = impl_->plural_.find(std::string(msgid));
        if (it != impl_->plural_.end()) {
            int index = evaluate_plural(impl_->plural_forms_, static_cast<std::int64_t>(n));
            if (index < 0 || index >= impl_->nplurals_) {
                index = n == 1 ? 0 : 1;
            }
            std::string_view const form = nul_field(it->second, index);
            if (!form.empty()) {
                return std::string(form);
            }
        }
    }
    return n == 1 ? std::string(msgid) : std::string(msgid_plural);
}

bool Translation::has_translation(std::string_view msgid) const
{
    if (impl_ == nullptr) {
        return false;
    }
    std::string const key(msgid);
    return impl_->singular_.find(key) != impl_->singular_.end() ||
           impl_->plural_.find(key) != impl_->plural_.end();
}

static std::mutex g_translation_mutex;
static std::unique_ptr<Translation> g_global_translation;

void Translation::set_global(std::unique_ptr<Translation> t)
{
    std::lock_guard<std::mutex> lock(g_translation_mutex);
    g_global_translation = std::move(t);
}

const Translation *Translation::global() { return g_global_translation.get(); }

std::string Translation::tr(std::string_view msgid)
{
    const Translation *const global = Translation::global();
    return global != nullptr ? global->translate(msgid) : std::string(msgid);
}

std::string Translation::tr(std::string_view msgid, std::string_view msgctxt)
{
    const Translation *const global = Translation::global();
    return global != nullptr ? global->translate(msgid, msgctxt) : std::string(msgid);
}

std::string Translation::ntr(std::string_view msgid, std::string_view msgid_plural, uint64_t n)
{
    const Translation *const global = Translation::global();
    return global != nullptr ? global->ntranslate(msgid, msgid_plural, n)
                             : (n == 1 ? std::string(msgid) : std::string(msgid_plural));
}

}  // namespace aegir::trinket
