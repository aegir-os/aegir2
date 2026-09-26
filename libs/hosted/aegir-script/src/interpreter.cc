/*
 * The shell's interpreter core (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/script/interpreter.h>

#include <utility>

namespace aegir::script {

namespace {

bool is_blank(char c) noexcept
{
    return c == ' ' || c == '\t';
}

char lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/* The first word of a line as a view, blanks around it skipped. */
std::string_view first_word(std::string_view line) noexcept
{
    std::size_t start = 0;
    while (start < line.size() && is_blank(line[start])) {
        ++start;
    }
    std::size_t end = start;
    while (end < line.size() && !is_blank(line[end])) {
        ++end;
    }
    return line.substr(start, end - start);
}

/* The text after the first word, leading blanks skipped. */
std::string_view after_first_word(std::string_view line) noexcept
{
    std::size_t i = 0;
    while (i < line.size() && is_blank(line[i])) {
        ++i;
    }
    while (i < line.size() && !is_blank(line[i])) {
        ++i;
    }
    while (i < line.size() && is_blank(line[i])) {
        ++i;
    }
    std::string_view rest = line.substr(i);
    while (!rest.empty() && is_blank(rest.back())) {
        rest.remove_suffix(1);
    }
    return rest;
}

bool equals_folded(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string control_word(std::string_view line)
{
    std::string_view const word = first_word(line);
    if (equals_folded(word, "lab") || equals_folded(word, "label")) {
        return "lab";
    }
    if (equals_folded(word, "skip")) {
        return "skip";
    }
    if (equals_folded(word, "if")) {
        return "if";
    }
    if (equals_folded(word, "else")) {
        return "else";
    }
    if (equals_folded(word, "endif")) {
        return "endif";
    }
    return {};
}

std::vector<std::string> script_lines(std::string_view text)
{
    std::vector<std::string> lines;
    std::string current;

    auto flush = [&]() {
        if (!current.empty() && current.back() == '\r') {
            current.pop_back();
        }
        std::size_t first = 0;
        while (first < current.size() && is_blank(current[first])) {
            ++first;
        }
        if (first < current.size() && current[first] != ';') {
            lines.push_back(current);
        }
        current.clear();
    };

    for (char const c : text) {
        if (c == '\n') {
            flush();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        flush();
    }
    return lines;
}

bool Frames::push(std::string path, std::vector<std::string> lines,
                  Arguments arguments)
{
    if (!path.empty()) {
        for (Frame const &frame : frames_) {
            if (frame.path == path) {
                return false;
            }
        }
    }
    frames_.push_back(
        Frame{std::move(path), std::move(lines), std::move(arguments), 0});
    return true;
}

std::string const *Frames::next()
{
    while (!frames_.empty()) {
        Frame &frame = frames_.back();
        if (frame.next >= frame.lines.size()) {
            frames_.pop_back();
            continue;
        }
        return &frame.lines[frame.next++];
    }
    return nullptr;
}

Arguments const *Frames::current_arguments() const noexcept
{
    return frames_.empty() ? nullptr : &frames_.back().arguments;
}

bool Frames::jump_to_label(std::string const &name)
{
    if (frames_.empty()) {
        return false;
    }
    Frame &frame = frames_.back();
    for (std::size_t i = 0; i < frame.lines.size(); ++i) {
        if (control_word(frame.lines[i]) != "lab") {
            continue;
        }
        if (equals_folded(after_first_word(frame.lines[i]), name)) {
            frame.next = i + 1;
            return true;
        }
    }
    return false;
}

bool Frames::skip_to_else_or_endif()
{
    if (frames_.empty()) {
        return false;
    }
    Frame &frame = frames_.back();
    std::size_t depth = 0;
    for (std::size_t i = frame.next; i < frame.lines.size(); ++i) {
        std::string const word = control_word(frame.lines[i]);
        if (word == "if") {
            ++depth;
        } else if (word == "endif") {
            if (depth == 0) {
                frame.next = i + 1;
                return true;
            }
            --depth;
        } else if (word == "else" && depth == 0) {
            frame.next = i + 1;
            return true;
        }
    }
    return false;
}

bool Frames::skip_to_endif()
{
    if (frames_.empty()) {
        return false;
    }
    Frame &frame = frames_.back();
    std::size_t depth = 0;
    for (std::size_t i = frame.next; i < frame.lines.size(); ++i) {
        std::string const word = control_word(frame.lines[i]);
        if (word == "if") {
            ++depth;
        } else if (word == "endif") {
            if (depth == 0) {
                frame.next = i + 1;
                return true;
            }
            --depth;
        }
    }
    return false;
}

bool Frames::abort()
{
    if (frames_.empty()) {
        return false;
    }
    frames_.pop_back();
    return true;
}

}  // namespace aegir::script
