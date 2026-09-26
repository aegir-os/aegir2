/*
 * The shell's interpreter core (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A command file is a sequence of lines; running one is a sequence of those
 * lines, where a spawned command completes in the middle and the next line
 * must then run. The shell does not loop for that: it holds a stack of
 * frames, one per active command file, and takes the next line from the top
 * before it reads the console. Execute pushes a frame, Quit and a FailAt
 * abort pop one, and a command file that names itself is a cycle, refused
 * rather than recursed.
 *
 * Hosted C++ (std::string), no seL4 and no allocation policy of its own; the
 * pure parts are asserted by scripts/check_script.py.
 */

#ifndef AEGIR_SCRIPT_INTERPRETER_H
#define AEGIR_SCRIPT_INTERPRETER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aegir::script {

/** The executable lines of a command file. A line whose first non-blank
 *  character is ';' is a comment, and a blank line is nothing; both are
 *  dropped. A trailing CR is stripped, so a CRLF file reads as an LF one.
 *  A kept line is otherwise its own text. */
std::vector<std::string> script_lines(std::string_view text);

/** One active command file: its path (for the cycle guard) and its cursor. */
struct Frame {
    std::string path;
    std::vector<std::string> lines;
    std::size_t next = 0;
};

/** The frame stack of a running interpreter. */
class Frames {
public:
    /** Push a command file. A path already active is a cycle and is refused;
     *  the guard is detection, not a depth cap. An empty path (an Eval line,
     *  not a file) is always allowed. */
    bool push(std::string path, std::vector<std::string> lines);

    /** The next line to run, or nullptr when no frame has one left -- the
     *  console's turn. A frame whose cursor has reached its end is dropped on
     *  the way out. */
    std::string const *next();

    /** Drop the innermost frame -- Quit, or a fail-level abort. False when no
     *  command file is running. */
    bool abort();

    bool empty() const noexcept { return frames_.empty(); }

    /** FailAt (specs/shell.md): the level at or above which a return code
     *  aborts the running script. The Amiga's default is 10, so a warning (5)
     *  does not stop a script and an error (10) does. A level of 0 never
     *  aborts. */
    std::uint64_t fail_level() const noexcept { return fail_level_; }
    void set_fail_level(std::uint64_t level) noexcept { fail_level_ = level; }
    static bool fails(std::uint64_t status, std::uint64_t fail_level) noexcept
    {
        return fail_level != 0 && status >= fail_level;
    }

private:
    std::vector<Frame> frames_;
    std::uint64_t fail_level_ = 10;
};

}  // namespace aegir::script

#endif  // AEGIR_SCRIPT_INTERPRETER_H
