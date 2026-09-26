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

}  // namespace

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

bool Frames::push(std::string path, std::vector<std::string> lines)
{
    if (!path.empty()) {
        for (Frame const &frame : frames_) {
            if (frame.path == path) {
                return false;
            }
        }
    }
    frames_.push_back(Frame{std::move(path), std::move(lines), 0});
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

bool Frames::abort()
{
    if (frames_.empty()) {
        return false;
    }
    frames_.pop_back();
    return true;
}

}  // namespace aegir::script
