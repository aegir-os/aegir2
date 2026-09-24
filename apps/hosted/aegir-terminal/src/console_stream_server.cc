/*
 * ConsoleStreamServer implementation (specs/terminal.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "console_stream_server.h"

#include <aegir/console_stream.h>
#include <aegir/trinket/unicode.h>

#include <utility>

namespace aegir::terminal {

using aegir::trinket::LineEditor;
using aegir::trinket::TerminalBuffer;
using aegir::trinket::utf32_to_utf8;
using aegir::trinket::utf8_to_utf32;

ConsoleStreamServer::ConsoleStreamServer(TerminalBuffer& buffer) : buffer_(buffer) {}

ConsoleStreamServer::Stream* ConsoleStreamServer::find(uint64_t caller)
{
    auto const it = streams_.find(caller);
    return it == streams_.end() ? nullptr : &it->second;
}

ConsoleStreamServer::Stream const* ConsoleStreamServer::find(uint64_t caller) const
{
    auto const it = streams_.find(caller);
    return it == streams_.end() ? nullptr : &it->second;
}

bool ConsoleStreamServer::open_stream(uint64_t caller, uint32_t mode,
                                      std::u32string prompt)
{
    if (find(caller) != nullptr) {
        return false;
    }
    Stream stream;
    stream.mode = mode;
    stream.prompt = std::move(prompt);
    if (mode == aegir::console::kStreamModeCooked) {
        stream.editor = std::make_unique<LineEditor>(buffer_);
        stream.editor->set_prompt(stream.prompt);
        /* Enter ends the line and the editor stops editing; the line waits
         * here for the client's read_line. The client begins the next prompt
         * once it has run the line, so a command's output lands between. */
        stream.editor->set_on_line([this, caller](std::u32string const& line) {
            Stream* s = find(caller);
            if (s != nullptr) {
                s->pending = line;
                s->ready = true;
            }
        });
    }
    streams_.emplace(caller, std::move(stream));
    return true;
}

bool ConsoleStreamServer::open_local(uint64_t caller, std::string_view prompt)
{
    return open_stream(caller, aegir::console::kStreamModeCooked,
                       utf8_to_utf32(prompt));
}

uint32_t ConsoleStreamServer::write_stream(uint64_t caller, std::string_view text)
{
    if (find(caller) == nullptr) {
        return 0;
    }
    buffer_.write(text);
    buffer_.scroll_to_bottom();
    if (on_change) {
        on_change();
    }
    return static_cast<uint32_t>(text.size());
}

uint32_t ConsoleStreamServer::write_local(uint64_t caller, std::string_view text)
{
    return write_stream(caller, text);
}

void ConsoleStreamServer::set_prompt(uint64_t caller, std::string_view prompt)
{
    Stream* s = find(caller);
    if (s == nullptr || s->editor == nullptr) {
        return;
    }
    s->prompt = utf8_to_utf32(prompt);
    s->editor->set_prompt(s->prompt);
}

bool ConsoleStreamServer::line_ready(uint64_t caller) const
{
    Stream const* s = find(caller);
    return s != nullptr && s->ready;
}

std::u32string ConsoleStreamServer::take_line(uint64_t caller)
{
    Stream* s = find(caller);
    if (s == nullptr) {
        return {};
    }
    std::u32string line = std::move(s->pending);
    s->pending.clear();
    s->ready = false;
    return line;
}

void ConsoleStreamServer::begin(uint64_t caller)
{
    Stream* s = find(caller);
    if (s != nullptr && s->editor != nullptr && !s->editor->editing()) {
        s->editor->begin();
        if (on_change) {
            on_change();
        }
    }
}

LineEditor* ConsoleStreamServer::editor(uint64_t caller)
{
    Stream* s = find(caller);
    return s == nullptr ? nullptr : s->editor.get();
}

bool ConsoleStreamServer::command_finished(uint64_t caller) const
{
    Stream const* s = find(caller);
    return s != nullptr && s->finished;
}

uint64_t ConsoleStreamServer::exit_status(uint64_t caller) const
{
    Stream const* s = find(caller);
    return s == nullptr ? 0 : s->status;
}

void ConsoleStreamServer::clear_command(uint64_t caller)
{
    Stream* s = find(caller);
    if (s != nullptr) {
        s->finished = false;
        s->status = 0;
    }
}

uint32_t ConsoleStreamServer::handle(uint32_t method, uint64_t const* words,
                                     uint32_t count, uint64_t caller, uint64_t* reply,
                                     uint32_t capacity)
{
    namespace console = aegir::console;
    namespace nmspace = aegir::nmspace;

    switch (method) {
    case console::kStreamMethodOpen: {
        if (count < 1) {
            return 0;
        }
        char const* prompt = nullptr;
        uint32_t prompt_length = 0;
        if (!nmspace::unpack_string(words + 1, count - 1, console::kStreamBytesMax,
                                    &prompt, &prompt_length)) {
            return 0;
        }
        if (!open_stream(caller, static_cast<uint32_t>(words[0]),
                         utf8_to_utf32(std::string_view(prompt, prompt_length)))) {
            return 0;
        }
        if (capacity < 1) {
            return 0;
        }
        reply[0] = 1;
        return 1;
    }
    case console::kStreamMethodWrite: {
        char const* text = nullptr;
        uint32_t length = 0;
        if (!nmspace::unpack_string(words, count, console::kStreamBytesMax, &text,
                                    &length)) {
            return 0;
        }
        if (capacity < 1) {
            return 0;
        }
        reply[0] = write_stream(caller, std::string_view(text, length));
        return 1;
    }
    case console::kStreamMethodReadLine: {
        Stream* s = find(caller);
        if (s == nullptr || s->editor == nullptr) {
            return 0;
        }
        if (!s->ready && !s->editor->editing()) {
            s->editor->begin();
        }
        if (!s->ready) {
            return 0;
        }
        std::string const line = utf32_to_utf8(s->pending);
        s->pending.clear();
        s->ready = false;
        if (line.size() > console::kStreamBytesMax) {
            return 0;
        }
        return nmspace::pack_string(reply, line.data(), static_cast<uint32_t>(line.size()),
                                    console::kStreamBytesMax);
    }
    case console::kStreamMethodRead:
        /* Tier 1 is a poll and raw input is not queued yet: an empty answer. */
        return 0;
    case console::kStreamMethodClose: {
        Stream* s = find(caller);
        if (s != nullptr) {
            if (count >= 1) {
                s->status = words[0];
            }
            streams_.erase(caller);
        }
        return 0;
    }
    case console::kStreamMethodExit: {
        /* A command said it is done. The stream stays open -- it is the
         * shell's -- and the status waits for the terminal to finalize. */
        Stream* s = find(caller);
        if (s != nullptr) {
            if (count >= 1) {
                s->status = words[0];
            }
            s->finished = true;
        }
        return 0;
    }
    default:
        /* A method this version does not know is answered by saying nothing
         * (specs/services.md's versioning rule). */
        return 0;
    }
}

}  // namespace aegir::terminal
