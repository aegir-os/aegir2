/*
 * The Aegir shell: the command line a terminal runs (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The Amiga command line: a prompt that is the current directory, an implicit
 * change when a name is a directory rather than a command, and the built-in
 * commands that act on the shell's own state or the namespace. It reads and
 * writes through the terminal's console stream (`ConsoleStreamServer`), so its
 * output and a command's share the one grid; a command it does not know it
 * hands to the terminal's spawner, and stays busy until the command's exit.
 */

#ifndef AEGIR_TERMINAL_SHELL_H
#define AEGIR_TERMINAL_SHELL_H

#include "console_stream_server.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aegir::terminal {

class Shell {
public:
    Shell(ConsoleStreamServer& server, uint64_t stream, std::function<void()> quit);

    /* Open the cooked stream, print the banner, and draw the first prompt. */
    void start();

    /* One finished command line, taken from the stream. */
    void run_line(std::u32string const& line);

    /* How the shell runs a command it does not know: the terminal's, which
     * holds the spawn kit. It answers whether the command started; a started
     * command leaves the shell busy until command_finished. Parsing the line
     * is the shell's; the spawning is the terminal's. */
    void set_spawn(
        std::function<bool(std::string const&, std::vector<std::string> const&)> spawn)
    {
        spawn_ = std::move(spawn);
    }

    bool busy() const { return busy_; }

    /* A command's exit: print the status line when it is non-zero (the Amiga's
     * `return code N`) and draw the next prompt. */
    void command_finished(uint64_t status);

    /* The process's current directory, a VFS path: what a command inherits
     * (specs/environment.md). */
    std::string current_directory() const;

private:
    std::string prompt_for(std::string directory) const;
    void refresh_prompt();

    /* A command's argument as a full Volume:path, against the current
     * directory, with `.` and `..` handled the Amiga way (an empty component
     * is the parent). */
    std::string resolve(std::string const& arg) const;
    bool is_directory(std::string const& path) const;
    bool change_directory(std::string const& arg);

    void print(std::string const& text);
    void command_dir(std::string const& arg);
    void command_type(std::string const& arg);

    ConsoleStreamServer& server_;
    uint64_t stream_;
    std::function<void()> quit_;
    std::function<bool(std::string const&, std::vector<std::string> const&)> spawn_;
    bool busy_ = false;
};

}  // namespace aegir::terminal

#endif  // AEGIR_TERMINAL_SHELL_H
