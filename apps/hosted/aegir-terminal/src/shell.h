/*
 * The Aegir shell: the command line a terminal runs (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The Amiga command line: a prompt that is the current directory, an implicit
 * change when a name is a directory rather than a command, and the built-in
 * commands that act on the shell's own state or the namespace. It edits
 * through a LineEditor and prints through the TerminalBuffer the terminal
 * owns. Commands the shell does not know it does not run yet -- external
 * programs are the spawn arc's.
 */

#ifndef AEGIR_TERMINAL_SHELL_H
#define AEGIR_TERMINAL_SHELL_H

#include <aegir/trinket/line_editor.h>
#include <aegir/trinket/terminal_buffer.h>

#include <functional>
#include <string>
#include <vector>

namespace aegir::terminal {

class Shell {
public:
    Shell(aegir::trinket::LineEditor& editor, aegir::trinket::TerminalBuffer& buffer,
          std::function<void()> quit);

    /* Print the banner, set the prompt from the current directory, and draw
     * the first line. */
    void start();

    /* One finished command line: the LineEditor's callback. */
    void run_line(std::u32string const& line);

private:
    std::string current_directory() const;
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

    aegir::trinket::LineEditor& editor_;
    aegir::trinket::TerminalBuffer& buffer_;
    std::function<void()> quit_;
};

} // namespace aegir::terminal

#endif // AEGIR_TERMINAL_SHELL_H
