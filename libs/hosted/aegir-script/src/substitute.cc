/*
 * Variable substitution for the shell (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/script/substitute.h>

#include <cstddef>

namespace aegir::script {

namespace {

bool is_blank(char c) noexcept
{
    return c == ' ' || c == '\t';
}

bool is_name_char(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

bool all_digits(std::string const &text) noexcept
{
    if (text.empty()) {
        return false;
    }
    for (char const c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && is_blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_blank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool equals_folded(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char const ca = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
        char const cb = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

/* The first word of a line and the rest after it, each as a view: the alias
 * walk needs the raw text, not a substituted line. No quotes are honoured -- a
 * command or an alias name is not quoted. */
void split_word(std::string_view line, std::string_view &word,
                std::string_view &rest) noexcept
{
    std::size_t start = 0;
    while (start < line.size() && is_blank(line[start])) {
        ++start;
    }
    if (start == line.size()) {
        word = {};
        rest = {};
        return;
    }
    std::size_t end = start;
    while (end < line.size() && !is_blank(line[end])) {
        ++end;
    }
    word = line.substr(start, end - start);
    std::size_t after = end;
    while (after < line.size() && is_blank(line[after])) {
        ++after;
    }
    rest = line.substr(after);
}

}  // namespace

std::string const *Arguments::find_named(std::string_view name) const noexcept
{
    for (auto const &entry : named) {
        if (std::string_view(entry.first) == name) {
            return &entry.second;
        }
    }
    return nullptr;
}

bool is_key_line(std::string_view line) noexcept
{
    std::size_t i = 0;
    while (i < line.size() && is_blank(line[i])) {
        ++i;
    }
    std::string_view const rest = line.substr(i);
    if (rest.size() < 4 || rest[0] != '.') {
        return false;
    }
    if (!equals_folded(rest.substr(1, 3), "KEY")) {
        return false;
    }
    return rest.size() == 4 || is_blank(rest[4]);
}

bool parse_key_list(std::string_view line, std::vector<KeySymbol> &out,
                    std::string &error)
{
    if (!is_key_line(line)) {
        error = "not a .KEY line";
        return false;
    }
    std::size_t i = 0;
    while (i < line.size() && is_blank(line[i])) {
        ++i;
    }
    std::string_view const rest = line.substr(i + 4);

    std::size_t pos = 0;
    while (pos <= rest.size()) {
        std::size_t const comma = rest.find(',', pos);
        std::string_view const piece =
            trim(rest.substr(pos, comma == std::string_view::npos
                                      ? std::string_view::npos
                                      : comma - pos));
        if (!piece.empty()) {
            std::size_t const slash = piece.find('/');
            std::string_view const name =
                slash == std::string_view::npos ? piece : piece.substr(0, slash);
            if (name.empty()) {
                error = "a .KEY parameter needs a name";
                return false;
            }
            KeySymbol symbol;
            symbol.name.assign(name);
            if (slash != std::string_view::npos) {
                std::string_view const flag = piece.substr(slash + 1);
                if (equals_folded(flag, "A")) {
                    symbol.required = true;
                } else {
                    error = "unsupported .KEY flag /" + std::string(flag);
                    return false;
                }
            }
            out.push_back(std::move(symbol));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return true;
}

bool bind_keys(std::vector<KeySymbol> const &keys, std::string_view script_name,
               std::vector<std::string> const &supplied, Arguments &out,
               std::string &error)
{
    out.name.assign(script_name);
    out.positional = supplied;
    out.named.clear();
    std::size_t next = 0;
    for (KeySymbol const &key : keys) {
        std::string value;
        if (next < supplied.size()) {
            value = supplied[next];
            ++next;
        } else if (key.required) {
            error = "not enough arguments: " + key.name + " is required";
            return false;
        }
        out.named.emplace_back(key.name, std::move(value));
    }
    return true;
}

bool substitute_words(std::string_view line, Arguments const &arguments,
                      Lookup const &variables, std::vector<std::string> &words,
                      std::string &error)
{
    std::string current;
    bool quoted = false;
    bool started = false;

    auto push_word = [&]() {
        words.push_back(current);
        current.clear();
        started = false;
    };
    auto append_unquoted = [&](std::string_view text) {
        for (char const c : text) {
            if (is_blank(c)) {
                if (started) {
                    push_word();
                }
            } else {
                current.push_back(c);
                started = true;
            }
        }
    };
    auto expand = [&](std::string const &name) -> std::string {
        if (all_digits(name)) {
            std::size_t index = 0;
            for (char const c : name) {
                index = index * 10 + static_cast<std::size_t>(c - '0');
            }
            if (index == 0) {
                return arguments.name;
            }
            if (index <= arguments.positional.size()) {
                return arguments.positional[index - 1];
            }
            return {};
        }
        if (std::string const *named = arguments.find_named(name)) {
            return *named;
        }
        std::string value;
        if (variables(name, value)) {
            return value;
        }
        return {};
    };

    for (std::size_t i = 0; i < line.size(); ++i) {
        char const c = line[i];
        if (c == '"') {
            quoted = !quoted;
            started = true;
            continue;
        }
        if (c != '$' && c != '{') {
            if (!quoted && is_blank(c)) {
                if (started) {
                    push_word();
                }
            } else {
                current.push_back(c);
                started = true;
            }
            continue;
        }

        std::string name;
        if (c == '{') {
            std::size_t const close = line.find('}', i + 1);
            if (close == std::string_view::npos) {
                error = "unmatched brace";
                return false;
            }
            name.assign(line.substr(i + 1, close - (i + 1)));
            i = close;
        } else if (i + 1 < line.size() && line[i + 1] == '{') {
            std::size_t const close = line.find('}', i + 2);
            if (close == std::string_view::npos) {
                error = "unmatched brace";
                return false;
            }
            name.assign(line.substr(i + 2, close - (i + 2)));
            i = close;
        } else {
            std::size_t j = i + 1;
            while (j < line.size() && is_name_char(line[j])) {
                ++j;
            }
            if (j == i + 1) {
                current.push_back('$');
                started = true;
                continue;
            }
            name.assign(line.substr(i + 1, j - (i + 1)));
            i = j - 1;
        }

        std::string const value = expand(name);
        if (quoted) {
            current.append(value);
            started = true;
        } else {
            append_unquoted(value);
        }
    }

    if (quoted) {
        error = "unmatched quote";
        return false;
    }
    if (started) {
        push_word();
    }
    return true;
}

std::string expand_aliases(std::string_view line, AliasTable const &aliases)
{
    std::string expanded(line);
    std::vector<std::string> seen;
    for (;;) {
        std::string_view word;
        std::string_view rest;
        split_word(expanded, word, rest);
        if (word.empty()) {
            break;
        }
        bool stop = false;
        for (std::string const &already : seen) {
            if (equals_folded(already, word)) {
                stop = true;
                break;
            }
        }
        if (stop) {
            break;
        }
        std::string const *value = nullptr;
        for (auto const &alias : aliases) {
            if (equals_folded(alias.first, word)) {
                value = &alias.second;
                break;
            }
        }
        if (value == nullptr) {
            break;
        }
        seen.emplace_back(word);
        expanded = rest.empty() ? *value : *value + " " + std::string(rest);
    }
    return expanded;
}

}  // namespace aegir::script
