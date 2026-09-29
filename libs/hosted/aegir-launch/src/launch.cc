/*
 * The launch API, built (specs/launch.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The request is packed from the caller's own context, so a program starts
 * another the way `fork` does: it says the program and the arguments, and the
 * environment, current directory and stack ride along without it naming them.
 * The C++ face and the C face are here together; `aegir_launch_request` is the
 * one place a call is actually made, so both go through it.
 */

#include <aegir/launch_client.h>

#include <aegir/console_stream.h>
#include <aegir/environment.h>
#include <aegir/nmspace.h>

#include <string_view>

namespace aegir::launch {

namespace {

uint32_t text_length(char const *text) noexcept
{
    uint32_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

/* Append a string to the request buffer, refusing rather than overflowing the
 * envelope (the same arithmetic console_stream_client uses). An empty string
 * travels as a zero-length field, which is how the launcher reads "none". */
bool put_string(uint64_t *out, uint32_t &words, char const *text, uint32_t length)
{
    uint32_t const need = 1 + (length + 7) / 8;
    if (words + need > aegir::ipc::kMaxWords) {
        return false;
    }
    uint32_t const packed =
        aegir::nmspace::pack_string(out + words, text, length, aegir::nmspace::kPathMax);
    if (packed == 0) {
        return false;
    }
    words += packed;
    return true;
}

/* The three context strings every request carries. The stack ask follows the
 * request's own fields, so it is added by put_stack. */
bool put_context(uint64_t *out, uint32_t &words, Context const &context)
{
    return put_string(out, words, context.cwd.data(),
                      static_cast<uint32_t>(context.cwd.size())) &&
           put_string(out, words, context.environment.data(),
                      static_cast<uint32_t>(context.environment.size())) &&
           put_string(out, words, context.path.data(),
                      static_cast<uint32_t>(context.path.size()));
}

bool put_stack(uint64_t *out, uint32_t &words, Context const &context)
{
    if (words >= aegir::ipc::kMaxWords) {
        return false;
    }
    out[words++] = context.stack_pages;
    return true;
}

}  // namespace

aegir::ipc::Consumer launcher() noexcept
{
    return aegir::ipc::Consumer::find(kPortName, kPortNameLength);
}

Context caller_context()
{
    Context context;
    std::string_view const cwd = aegir::environment::current_dir();
    context.cwd.assign(cwd.data(), cwd.size());
    char const *const *const entries = aegir::environment::environ();
    for (uint32_t i = 0; entries[i] != nullptr; ++i) {
        context.environment.append(entries[i]);
        context.environment.push_back('\0');
    }
    /* The path and the stack ask are not the runtime's yet: empty and the
     * spawner's default. They ride in the request so the shape is whole and a
     * later runtime fills them from the process without the wire changing. */
    context.path.clear();
    context.stack_pages = 0;
    return context;
}

extern "C" int aegir_launch_request(uint32_t method, uint64_t const *words,
                                    uint32_t count) noexcept
{
    aegir::ipc::Consumer const port = launcher();
    if (!port.valid()) {
        return -1;
    }
    /* The caller's own con.stream, when it has one: the request's one
     * capability, which the launcher hands the command so its output lands
     * where the caller's does (specs/launch.md, specs/signal.md). A caller
     * with no stream sends none, and the launcher gives the command a view of
     * its own. */
    aegir::ipc::Consumer const stream = aegir::ipc::Consumer::find(
        aegir::console::kStreamPortName, aegir::console::kStreamPortNameLength);
    uint64_t answer[1] = {};
    aegir::ipc::WordsReply const reply = port.call_transfer(
        method, words, count, stream.capability(), answer, 1, nullptr);
    if (reply.error != 0 || reply.count != 1) {
        return -1;
    }
    return answer[0] == 1 ? 1 : 0;
}

bool spawn(char const *argv, uint32_t argv_length, uint64_t kind, char const *window,
           uint32_t window_length)
{
    Context const context = caller_context();
    uint64_t out[aegir::ipc::kMaxWords];
    uint32_t words = 0;
    out[words++] = kind;
    out[words++] = 0;
    if (!put_string(out, words, argv, argv_length) ||
        !put_context(out, words, context) ||
        !put_string(out, words, "", 0) || /* std_in empty: the console */
        !put_string(out, words, "", 0) || /* std_out empty: the console */
        !put_string(out, words, window, window_length) ||
        !put_stack(out, words, context)) {
        return false;
    }
    return aegir_launch_request(kMethodSpawn, out, words) == 1;
}

bool command(char const *argv, uint32_t argv_length, char const *std_in,
             uint32_t std_in_length, char const *std_out, uint32_t std_out_length,
             bool background)
{
    Context const context = caller_context();
    uint64_t out[aegir::ipc::kMaxWords];
    uint32_t words = 0;
    out[words++] = kKindCommand;
    out[words++] = background ? kFlagBackground : 0;
    if (!put_string(out, words, argv, argv_length) ||
        !put_context(out, words, context) ||
        !put_string(out, words, std_in, std_in_length) ||
        !put_string(out, words, std_out, std_out_length) ||
        !put_string(out, words, "", 0) || /* no window: a command has none */
        !put_stack(out, words, context)) {
        return false;
    }
    return aegir_launch_request(kMethodSpawn, out, words) == 1;
}

bool pipeline(Stage const *stages, uint32_t count)
{
    if (count == 0) {
        return false;
    }
    Context const context = caller_context();
    uint64_t out[aegir::ipc::kMaxWords];
    uint32_t words = 0;
    out[words++] = kKindCommand;
    out[words++] = 0;
    out[words++] = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (!put_string(out, words, stages[i].argv, stages[i].argv_length) ||
            !put_string(out, words, stages[i].std_in, stages[i].std_in_length) ||
            !put_string(out, words, stages[i].std_out, stages[i].std_out_length)) {
            return false;
        }
    }
    if (!put_context(out, words, context) || !put_stack(out, words, context)) {
        return false;
    }
    return aegir_launch_request(kMethodPipeline, out, words) == 1;
}

extern "C" int aegir_spawn(char *const argv[], char const *std_in, char const *std_out,
                           int background, char const *window, int kind) noexcept
{
    if (argv == nullptr || argv[0] == nullptr) {
        return -1;
    }
    /* Pack argv as the wire's one argv string: the words NUL-separated,
     * program first, exactly what a shell hands over. */
    std::string line;
    for (uint32_t i = 0; argv[i] != nullptr; ++i) {
        if (i != 0) {
            line.push_back('\0');
        }
        line.append(argv[i]);
    }
    Context const context = caller_context();
    uint64_t out[aegir::ipc::kMaxWords];
    uint32_t words = 0;
    out[words++] = static_cast<uint64_t>(kind);
    out[words++] = background ? kFlagBackground : 0;
    char const *const in = std_in != nullptr ? std_in : "";
    char const *const out_text = std_out != nullptr ? std_out : "";
    char const *const win = window != nullptr ? window : "";
    if (!put_string(out, words, line.data(), static_cast<uint32_t>(line.size())) ||
        !put_context(out, words, context) ||
        !put_string(out, words, in, text_length(in)) ||
        !put_string(out, words, out_text, text_length(out_text)) ||
        !put_string(out, words, win, text_length(win)) ||
        !put_stack(out, words, context)) {
        return -1;
    }
    return aegir_launch_request(kMethodSpawn, out, words);
}

}  // namespace aegir::launch
