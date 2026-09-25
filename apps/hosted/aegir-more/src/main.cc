/*
 * more: page text files (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A page is the terminal's text area less a row for the prompt, asked of the
 * stream so there is no page height baked in. After a page with more to show,
 * `more` prints `--More--` and blocks on a key: space or return shows the next
 * page, q quits. The key arrives through fd 0, which parks on the console
 * doorbell the terminal rings while the command runs (specs/terminal.md), so
 * the wait costs no CPU the way a poll loop would.
 */

#include <aegir/args.h>
#include <aegir/command.h>
#include <aegir/console_stream.h>
#include <aegir/console_stream_client.h>
#include <aegir/ipc/port.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

/* The terminal's text area less a row for the prompt, so a page is the
 * window's, not a constant. Zero when the command has no stream to ask. */
int page_rows() noexcept
{
    aegir::ipc::Consumer const port = aegir::ipc::Consumer::find(
        aegir::console::kStreamPortName, aegir::console::kStreamPortNameLength);
    uint32_t rows = 0;
    uint32_t columns = 0;
    if (port.valid() && aegir::console::stream_size(port, &rows, &columns) && rows > 1) {
        return static_cast<int>(rows) - 1; /* one row for the prompt */
    }
    return 0;
}

std::vector<std::string> read_lines(char const *path, bool &ok) noexcept
{
    std::vector<std::string> lines;
    std::FILE *const handle = std::fopen(path, "rb");
    if (handle == nullptr) {
        ok = false;
        return lines;
    }
    std::string line;
    int c = 0;
    while ((c = std::fgetc(handle)) != EOF) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(static_cast<char>(c));
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    std::fclose(handle);
    ok = true;
    return lines;
}

/* The next key, blocking: fd 0's read parks on the console doorbell (or polls
 * when there is none, in which case zero is end of input). */
char next_key() noexcept
{
    char key = 0;
    for (;;) {
        long const got = ::read(0, &key, 1);
        if (got != 0) {
            return got < 0 ? 0 : key;
        }
    }
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("more")) {
        std::_Exit(127);
    }
    aegir::args::Result const args = aegir::args::read("FILE/A", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "more: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    bool ok = false;
    std::vector<std::string> const lines = read_lines(args.value("FILE"), ok);
    if (!ok) {
        std::fprintf(stderr, "more: cannot open %s\n", args.value("FILE"));
        return 10;
    }
    int const page = page_rows();
    if (page <= 0) {
        for (std::string const &line : lines) {
            std::fputs(line.c_str(), stdout);
            std::fputc('\n', stdout);
        }
        return 0;
    }
    std::size_t at = 0;
    while (at < lines.size()) {
        int shown = 0;
        while (at < lines.size() && shown < page) {
            std::fputs(lines[at].c_str(), stdout);
            std::fputc('\n', stdout);
            ++at;
            ++shown;
        }
        if (at >= lines.size()) {
            break;
        }
        std::fputs("--More--", stdout);
        char const key = next_key();
        std::fputs("\r      \r", stdout);
        if (key == 'q' || key == 'Q') {
            break;
        }
    }
    return 0;
}
