/*
 * The Aegir shell (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "shell.h"

#include <aegir/environment.h>
#include <aegir/trinket/unicode.h>

#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace aegir::terminal {

using aegir::trinket::utf32_to_utf8;
using aegir::trinket::utf8_to_utf32;

namespace {

std::string to_lower(std::string text)
{
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::vector<std::string> split_words(std::u32string const& line)
{
    std::vector<std::string> words;
    std::u32string current;
    for (char32_t const cp : line) {
        if (cp == U' ' || cp == U'\t') {
            if (!current.empty()) {
                words.push_back(utf32_to_utf8(current));
                current.clear();
            }
        } else {
            current.push_back(cp);
        }
    }
    if (!current.empty()) {
        words.push_back(utf32_to_utf8(current));
    }
    return words;
}

std::string join_tail(std::string const& line, std::string const& first)
{
    std::size_t const at = line.find(first);
    if (at == std::string::npos) {
        return {};
    }
    std::size_t i = at + first.size();
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    return line.substr(i);
}

/* The parent of a Volume:path in the Amiga convention: an empty component is
 * the parent, and the parent of a volume root is itself. */
std::string parent_of(std::string const& path)
{
    if (path.empty() || path.back() == ':') {
        return path;
    }
    std::size_t const slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        std::size_t const colon = path.find(':');
        return colon == std::string::npos ? path : path.substr(0, colon + 1);
    }
    return path.substr(0, slash);
}

std::string append_component(std::string const& base, std::string const& component)
{
    if (base.empty() || base.back() == ':') {
        return base + component;
    }
    return base + "/" + component;
}

}  // namespace

Shell::Shell(ConsoleStreamServer& server, uint64_t stream, std::function<void()> quit)
    : server_(server), stream_(stream), quit_(std::move(quit)) {}

void Shell::print(std::string const& text)
{
    server_.write_local(stream_, text);
}

std::string Shell::current_directory() const
{
    std::error_code error;
    std::filesystem::path const path = std::filesystem::current_path(error);
    return error ? std::string("?") : path.string();
}

std::string Shell::prompt_for(std::string directory) const
{
    /* The Amiga prompt is the current directory and `>`: the volume-and-path
     * form, colon kept, so a login opens on `Home:>`. */
    return directory + ">";
}

void Shell::refresh_prompt()
{
    server_.set_prompt(stream_, prompt_for(current_directory()));
}

void Shell::start()
{
    server_.open_local(stream_, prompt_for(current_directory()));
    print("Aegir shell -- CD, Dir, Type, Echo, Quit\n");
    server_.begin(stream_);
}

std::string Shell::resolve(std::string const& arg) const
{
    if (arg.find(':') != std::string::npos) {
        return arg;
    }
    std::string base = current_directory();
    std::size_t i = 0;
    while (i <= arg.size()) {
        std::size_t const slash = arg.find('/', i);
        std::string const component =
            arg.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (component == ".." || component.empty()) {
            base = parent_of(base);
        } else if (component != ".") {
            base = append_component(base, component);
        }
        if (slash == std::string::npos) {
            break;
        }
        i = slash + 1;
    }
    return base;
}

bool Shell::is_directory(std::string const& path) const
{
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error;
}

bool Shell::change_directory(std::string const& arg)
{
    std::string const target = resolve(arg);
    if (!is_directory(target)) {
        return false;
    }
    std::error_code error;
    std::filesystem::current_path(target, error);
    if (error) {
        return false;
    }
    refresh_prompt();
    return true;
}

void Shell::command_dir(std::string const& arg)
{
    std::string const target = arg.empty() ? current_directory() : resolve(arg);
    std::error_code error;
    std::filesystem::directory_iterator it(target, error);
    if (error) {
        print("Dir: cannot list " + target + "\n");
        return;
    }
    std::filesystem::directory_iterator const end;
    int count = 0;
    for (; !error && it != end; it.increment(error)) {
        std::error_code kind_error;
        std::string const name = it->path().filename().string();
        bool const directory = it->is_directory(kind_error);
        print(name + (directory ? "/\n" : "\n"));
        ++count;
    }
    print(std::to_string(count) + (count == 1 ? " entry\n" : " entries\n"));
}

void Shell::command_type(std::string const& arg)
{
    if (arg.empty()) {
        print("Type: what file?\n");
        return;
    }
    std::string const target = resolve(arg);
    std::FILE* const file = std::fopen(target.c_str(), "rb");
    if (file == nullptr) {
        print("Type: cannot open " + target + "\n");
        return;
    }
    char chunk[512];
    std::size_t const have = std::fread(chunk, 1, sizeof(chunk), file);
    if (have > 0) {
        server_.write_local(stream_, std::string_view(chunk, have));
    }
    std::fclose(file);
}

void Shell::command_set(std::string const& arg)
{
    std::size_t const space = arg.find_first_of(" \t");
    std::string const name = space == std::string::npos ? arg : arg.substr(0, space);
    std::string value;
    if (space != std::string::npos) {
        std::size_t const start = arg.find_first_not_of(" \t", space);
        if (start != std::string::npos) {
            value = arg.substr(start);
        }
    }
    if (name.empty() || !aegir::environment::setenv(name.c_str(), value.c_str())) {
        print("Set: a name and a value, please\n");
    }
}

void Shell::command_get(std::string const& arg)
{
    std::size_t const space = arg.find_first_of(" \t");
    std::string const name = space == std::string::npos ? arg : arg.substr(0, space);
    if (name.empty()) {
        print("Get: what variable?\n");
        return;
    }
    char const* const value = aegir::environment::getenv(name.c_str());
    print(name + "=" + (value != nullptr ? value : "(not set)") + "\n");
}

void Shell::run_line(std::u32string const& line)
{
    std::vector<std::string> const words = split_words(line);
    if (words.empty()) {
        server_.begin(stream_);
        return;
    }
    std::string const command = to_lower(words[0]);
    std::string const text = utf32_to_utf8(line);
    std::string const arg = join_tail(text, words[0]);

    if (command == "cd" || command == "currentdir") {
        if (arg.empty()) {
            print(current_directory() + "\n");
        } else if (!change_directory(arg)) {
            print("CD: not a directory: " + arg + "\n");
        }
    } else if (command == "dir" || command == "list") {
        command_dir(arg);
    } else if (command == "type") {
        command_type(arg);
    } else if (command == "echo") {
        print(arg + "\n");
    } else if (command == "set" || command == "setvar") {
        command_set(arg);
    } else if (command == "get" || command == "getvar") {
        command_get(arg);
    } else if (command == "quit" || command == "endcli") {
        print("bye\n");
        if (quit_) {
            quit_();
        }
        return;
    } else if (is_directory(resolve(words[0]))) {
        /* The Amiga's implicit change: a name that is not a command but is a
         * directory becomes the current directory. */
        (void)change_directory(words[0]);
    } else {
        /* Not a built-in and not a directory: a program, looked up by name
         * (specs/shell.md). A started command leaves the shell busy; its exit
         * draws the next prompt (command_finished). */
        std::vector<std::string> const args(words.begin() + 1, words.end());
        if (spawn_ && spawn_(words[0], args)) {
            busy_ = true;
            return;
        }
        print("Unknown command: " + words[0] + "\n");
    }

    server_.begin(stream_);
}

void Shell::command_finished(uint64_t status)
{
    busy_ = false;
    if (status != 0) {
        print("return code " + std::to_string(status) + "\n");
    }
    server_.begin(stream_);
}

} // namespace aegir::terminal
