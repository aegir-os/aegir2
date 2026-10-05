/*
 * Sys:S/network.manifest, parsed. See manifest.h and specs/net.md.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "manifest.h"

#include <aegir/resolve.h>

namespace netconfig {
namespace {

bool is_space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r';
}

void trim(char const *&begin, char const *&end) noexcept
{
    while (begin < end && is_space(*begin)) {
        ++begin;
    }
    while (end > begin && is_space(*(end - 1))) {
        --end;
    }
}

bool equals(char const *begin, char const *end, char const *text) noexcept
{
    uint32_t i = 0;
    for (; begin < end; ++begin, ++i) {
        if (text[i] == '\0' || *begin != text[i]) {
            return false;
        }
    }
    return text[i] == '\0';
}

bool boolean(char const *begin, char const *end, bool *value) noexcept
{
    if (equals(begin, end, "true")) {
        *value = true;
        return true;
    }
    if (equals(begin, end, "false")) {
        *value = false;
        return true;
    }
    return false;
}

/* "key = value", the key up to the first '=' and the value the rest, both
 * trimmed. */
bool split(char const *begin, char const *end, char const *&key_begin,
           char const *&key_end, char const *&value_begin, char const *&value_end) noexcept
{
    char const *equals_at = begin;
    while (equals_at < end && *equals_at != '=') {
        ++equals_at;
    }
    if (equals_at == end) {
        return false;
    }
    key_begin = begin;
    key_end = equals_at;
    value_begin = equals_at + 1;
    value_end = end;
    trim(key_begin, key_end);
    trim(value_begin, value_end);
    return key_begin != key_end;
}

/* A dotted quad over a range: copy it NUL-terminated and let the resolver's
 * parser read it (the same one ping sorts an argument with). Zero is a bad
 * address, and there is no valid one it rejects. */
uint32_t address_of(char const *begin, char const *end) noexcept
{
    char text[16];
    uint32_t const length = static_cast<uint32_t>(end - begin);
    if (length == 0 || length >= sizeof(text)) {
        return 0;
    }
    for (uint32_t i = 0; i < length; ++i) {
        text[i] = begin[i];
    }
    text[length] = '\0';
    return aegir::resolve::parse_ipv4(text);
}

}  // namespace

Manifest::Manifest() noexcept
    : handler_{nullptr, nullptr, nullptr}, problem_{0, nullptr}, line_length_(0),
      overlong_(false), line_number_(1), format_seen_(false), hostname_seen_(false),
      in_section_(false), current_{}
{
}

void Manifest::set_handler(Handler const &handler) noexcept
{
    handler_ = handler;
}

bool Manifest::fail(char const *message) noexcept
{
    problem_.line = line_number_;
    problem_.message = message;
    return false;
}

void Manifest::emit_current() noexcept
{
    if (handler_.adapter != nullptr) {
        handler_.adapter(handler_.context, current_);
    }
}

bool Manifest::line(char const *begin, char const *end) noexcept
{
    trim(begin, end);
    if (begin == end || *begin == '#') {
        return true;
    }

    if (*begin == '[') {
        if (*(end - 1) != ']') {
            return fail("a section header must be [name]");
        }
        char const *name_begin = begin + 1;
        char const *name_end = end - 1;
        trim(name_begin, name_end);
        if (name_begin == name_end) {
            return fail("a section header must be [name]");
        }
        if (!format_seen_) {
            return fail("`format` must come before the first section");
        }
        uint32_t const name_length = static_cast<uint32_t>(name_end - name_begin);
        if (name_length > kNameMax) {
            return fail("an adapter name is too long");
        }
        /* A new header ends the section before it, which is when its settings
         * are complete and are handed on. */
        if (in_section_) {
            emit_current();
        }
        current_ = Adapter{};
        for (uint32_t i = 0; i < name_length; ++i) {
            current_.name[i] = name_begin[i];
        }
        current_.name[name_length] = '\0';
        current_.name_length = name_length;
        in_section_ = true;
        return true;
    }

    char const *key_begin = nullptr;
    char const *key_end = nullptr;
    char const *value_begin = nullptr;
    char const *value_end = nullptr;
    if (!split(begin, end, key_begin, key_end, value_begin, value_end)) {
        return fail("expected `key = value`, or a [section]");
    }

    if (!in_section_) {
        if (equals(key_begin, key_end, "format")) {
            if (!equals(value_begin, value_end, "1")) {
                return fail("this is not a manifest version this reader understands");
            }
            format_seen_ = true;
            return true;
        }
        if (equals(key_begin, key_end, "hostname")) {
            uint32_t const length = static_cast<uint32_t>(value_end - value_begin);
            if (length == 0 || length > kHostnameMax) {
                return fail("hostname is 1 to 63 bytes (the DNS label's ceiling)");
            }
            if (hostname_seen_) {
                return fail("hostname is declared twice");
            }
            hostname_seen_ = true;
            if (handler_.hostname != nullptr) {
                handler_.hostname(handler_.context, value_begin, length);
            }
            return true;
        }
        return fail("only `format` and `hostname` may come before the first section");
    }

    if (equals(key_begin, key_end, "dhcp")) {
        if (!boolean(value_begin, value_end, &current_.dhcp)) {
            return fail("dhcp is either `true` or `false`");
        }
        return true;
    }
    if (equals(key_begin, key_end, "ipv4_address")) {
        current_.address = address_of(value_begin, value_end);
        if (current_.address == 0) {
            return fail("ipv4_address is a dotted quad");
        }
        current_.has_address = true;
        return true;
    }
    if (equals(key_begin, key_end, "ipv4_netmask")) {
        current_.netmask = address_of(value_begin, value_end);
        if (current_.netmask == 0) {
            return fail("ipv4_netmask is a dotted quad");
        }
        current_.has_netmask = true;
        return true;
    }
    if (equals(key_begin, key_end, "ipv4_gateway")) {
        current_.gateway = address_of(value_begin, value_end);
        if (current_.gateway == 0) {
            return fail("ipv4_gateway is a dotted quad");
        }
        current_.has_gateway = true;
        return true;
    }
    return fail("unknown key");
}

bool Manifest::feed(char const *text, uint32_t length) noexcept
{
    for (uint32_t i = 0; i < length; ++i) {
        char const c = text[i];
        if (c == '\n') {
            if (overlong_) {
                return fail("a line is longer than the reader holds");
            }
            if (!line(line_, line_ + line_length_)) {
                return false;
            }
            line_length_ = 0;
            ++line_number_;
        } else if (line_length_ < kLineMax) {
            line_[line_length_++] = c;
        } else {
            overlong_ = true;
        }
    }
    return true;
}

bool Manifest::finish() noexcept
{
    if (overlong_) {
        return fail("a line is longer than the reader holds");
    }
    if (line_length_ > 0) {
        if (!line(line_, line_ + line_length_)) {
            return false;
        }
        line_length_ = 0;
        ++line_number_;
    }
    if (in_section_) {
        emit_current();
        in_section_ = false;
    }
    if (!format_seen_) {
        return fail("`format` is missing");
    }
    return true;
}

}  // namespace netconfig
