/*
 * The CON: handler's stream server (specs/terminal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The handler side of `aegir/console_stream.h`: it owns the streams, one per
 * client badge, and each cooked stream's line editor. The wire method is
 * `handle`; the terminal's own shell -- which is in the same process for now,
 * so it cannot call its own endpoint -- reaches the same Stream through the
 * `*_local` methods. One Stream, one implementation, whichever side asks.
 */

#ifndef AEGIR_TERMINAL_CONSOLE_STREAM_SERVER_H
#define AEGIR_TERMINAL_CONSOLE_STREAM_SERVER_H

#include <aegir/trinket/line_editor.h>
#include <aegir/trinket/terminal_buffer.h>
#include <aegir/trinket/widget.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aegir::terminal {

class ConsoleStreamServer {
public:
    explicit ConsoleStreamServer(aegir::trinket::TerminalBuffer& buffer);

    /* Called after the grid changed -- a write landed or a prompt was drawn --
     * so the terminal can damage the view that shows it. The buffer is a pure
     * value and knows no view; the handler is the one place both meet. */
    std::function<void()> on_change;

    /* The wire: one call from a stream's client. The reply words land in
     * `reply` (up to `capacity`); the answer is how many were written. */
    uint32_t handle(uint32_t method, uint64_t const* words, uint32_t count,
                    uint64_t caller, uint64_t* reply, uint32_t capacity);

    /* The terminal's own shell, in-process: the same handler, reached
     * directly. A process cannot call its own endpoint. */
    bool open_local(uint64_t caller, std::string_view prompt);
    uint32_t write_local(uint64_t caller, std::string_view text);
    void set_prompt(uint64_t caller, std::string_view prompt);
    bool line_ready(uint64_t caller) const;
    std::u32string take_line(uint64_t caller);
    void begin(uint64_t caller);
    aegir::trinket::LineEditor* editor(uint64_t caller);

    /* Raw input (specs/terminal.md): a key that reaches a raw stream -- or a
     * cooked stream while a command runs -- is a byte on the stream's input
     * queue, and the client drains it with `read`. The queue grows on demand;
     * tier 1 is a poll and the handler signals no doorbell yet. */
    void queue_input(uint64_t caller, std::string_view bytes);
    uint32_t read_input(uint64_t caller, char* out, uint32_t capacity);
    bool has_input(uint64_t caller) const;

    /* The bracket a command runs in (specs/shell.md's Phase 4, design A): the
     * command inherits the shell's stream, and while it runs the terminal
     * routes keys to the input queue rather than the idle editor. */
    void begin_command(uint64_t caller);
    bool in_command(uint64_t caller) const;

    /* One key, routed by the stream's discipline: a command bracket or a raw
     * stream queues it as bytes; an idle cooked stream feeds the editor. True
     * when the key was consumed. */
    bool on_key(uint64_t caller, aegir::trinket::KeyEvent const& event);

    /* A command's end-of-run report (`kStreamMethodExit`): the shell's stream
     * carries a status and a flag once a command has said it is done, which is
     * what the terminal finalizes on. */
    bool command_finished(uint64_t caller) const;
    uint64_t exit_status(uint64_t caller) const;
    void clear_command(uint64_t caller);

private:
    struct Stream {
        uint32_t mode = 0;
        std::unique_ptr<aegir::trinket::LineEditor> editor;
        std::u32string prompt;
        std::u32string pending;
        std::string input;
        bool ready = false;
        bool command = false;
        bool finished = false;
        uint64_t status = 0;
    };

    Stream* find(uint64_t caller);
    Stream const* find(uint64_t caller) const;
    bool open_stream(uint64_t caller, uint32_t mode, std::u32string prompt);
    uint32_t write_stream(uint64_t caller, std::string_view text);
    bool queue_key(uint64_t caller, aegir::trinket::KeyEvent const& event);

    aegir::trinket::TerminalBuffer& buffer_;
    std::unordered_map<uint64_t, Stream> streams_;
};

}  // namespace aegir::terminal

#endif  // AEGIR_TERMINAL_CONSOLE_STREAM_SERVER_H
