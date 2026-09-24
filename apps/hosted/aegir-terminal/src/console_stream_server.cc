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
                if (on_wake) {
                    on_wake(caller);
                }
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
        s->command = false;
        s->status = 0;
    }
}

void ConsoleStreamServer::queue_input(uint64_t caller, std::string_view bytes)
{
    Stream* s = find(caller);
    if (s != nullptr) {
        s->input.append(bytes);
        if (on_wake) {
            on_wake(caller);
        }
    }
}

uint32_t ConsoleStreamServer::read_input(uint64_t caller, char* out, uint32_t capacity)
{
    Stream* s = find(caller);
    if (s == nullptr) {
        return 0;
    }
    uint32_t const count =
        s->input.size() < capacity ? static_cast<uint32_t>(s->input.size()) : capacity;
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = s->input[i];
    }
    s->input.erase(0, count);
    return count;
}

bool ConsoleStreamServer::has_input(uint64_t caller) const
{
    Stream const* s = find(caller);
    return s != nullptr && !s->input.empty();
}

void ConsoleStreamServer::begin_command(uint64_t caller)
{
    Stream* s = find(caller);
    if (s != nullptr) {
        s->command = true;
    }
}

bool ConsoleStreamServer::in_command(uint64_t caller) const
{
    Stream const* s = find(caller);
    return s != nullptr && s->command;
}

void ConsoleStreamServer::set_doorbell(uint64_t caller, uint64_t slot)
{
    Stream* s = find(caller);
    if (s != nullptr) {
        s->doorbell = slot;
    }
}

uint64_t ConsoleStreamServer::doorbell(uint64_t caller) const
{
    Stream const* s = find(caller);
    return s == nullptr ? 0 : s->doorbell;
}

bool ConsoleStreamServer::on_key(uint64_t caller, aegir::trinket::KeyEvent const& event)
{
    Stream* s = find(caller);
    if (s == nullptr) {
        return false;
    }
    if (s->command || s->mode == aegir::console::kStreamModeRaw) {
        return queue_key(caller, event);
    }
    return s->editor != nullptr && s->editor->on_key(event);
}

bool ConsoleStreamServer::queue_key(uint64_t caller, aegir::trinket::KeyEvent const& event)
{
    if (!event.pressed) {
        return false;
    }
    switch (event.code) {
    case aegir::trinket::KeyCode::ENTER:
        queue_input(caller, "\r");
        return true;
    case aegir::trinket::KeyCode::BACKSPACE:
        queue_input(caller, "\b");
        return true;
    case aegir::trinket::KeyCode::TAB:
        queue_input(caller, "\t");
        return true;
    default:
        break;
    }
    if (event.text >= 32 && event.text != 0x7F) {
        queue_input(caller, utf32_to_utf8(std::u32string(1, event.text)));
        return true;
    }
    return false;
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
    case console::kStreamMethodRead: {
        /* Tier 1 is a poll: answer with whatever input is queued, or an empty
         * answer. The reply's own room bounds the bytes, so a client with more
         * than that to drain reads in pieces. */
        if (find(caller) == nullptr || capacity < 1) {
            return 0;
        }
        uint32_t const room = (capacity - 1) * 8;
        uint32_t const bound =
            room < console::kStreamBytesMax ? room : console::kStreamBytesMax;
        char buffer[console::kStreamBytesMax];
        uint32_t const got = read_input(caller, buffer, bound);
        if (got == 0) {
            return 0;
        }
        return aegir::nmspace::pack_string(reply, buffer, got, console::kStreamBytesMax);
    }
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
            if (on_wake) {
                on_wake(caller);
            }
        }
        return 0;
    }
    case console::kStreamMethodSetPrompt: {
        char const* prompt = nullptr;
        uint32_t length = 0;
        if (!nmspace::unpack_string(words, count, console::kStreamBytesMax, &prompt,
                                    &length)) {
            return 0;
        }
        set_prompt(caller, std::string_view(prompt, length));
        return 0;
    }
    case console::kStreamMethodCommandStatus: {
        /* The shell reads a finished command's status; reading it clears the
         * finished state and ends the command bracket (keys go back to the
         * editor). An empty answer means no command has finished. */
        Stream* s = find(caller);
        if (s == nullptr || !s->finished || capacity < 1) {
            return 0;
        }
        reply[0] = s->status;
        s->finished = false;
        s->command = false;
        return 1;
    }
    default:
        /* A method this version does not know is answered by saying nothing
         * (specs/services.md's versioning rule). */
        return 0;
    }
}

}  // namespace aegir::terminal
