/*
 * aegir-terminal: the session's CON: handler and command line.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The terminal owns one console window and a text grid, and runs the shell in
 * it (specs/terminal.md, specs/shell.md). Auth starts it beside the bureau at
 * login, with the console, the namespace and its own Home current directory.
 * It is the first slice a person can use: the built-in commands act on the
 * shell's state and the namespace, and a name that is a directory changes the
 * current directory the Amiga way.
 *
 * The CON: stream and the shell as a separate process -- so an arbitrary
 * command can print -- are the next phases; this process owns both sides for
 * now, behind the LineEditor that the stream will serve.
 */

#include "shell.h"

#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/line_editor.h>
#include <aegir/trinket/terminal_view.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>
#include <aegir/trinket/window.h>
#include <sel4/sel4.h>

#include <memory>
#include <string>

namespace {

void write(char const *text)
{
    aegir::debug_write(text);
}

/* Clear of the test bed's windows, the demo, and the screen bar's samples. */
constexpr int kWindowX = 40;
constexpr int kWindowY = 120;
constexpr int kWindowWidth = 560;
constexpr int kWindowHeight = 380;

}  // namespace

int main(int argc, char *argv[])
{
    using namespace aegir::trinket;

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    Application& app = Application::create(argc, argv);

    aegir::ipc::Consumer const gui = aegir::ipc::Consumer::find(
        aegir::console::kPortName, aegir::console::kPortNameLength);
    if (!gui.valid()) {
        write("  terminal: FAIL no console.gui\n");
        return 1;
    }
    app.set_gui_port(gui);

    Window window(app);
    window.set_title("Terminal");
    window.set_rect({kWindowX, kWindowY, kWindowWidth, kWindowHeight});

    auto view = std::make_unique<TerminalView>();
    view->set_font(app.default_font());
    view->set_colors(app.theme().color(ColorRole::TEXT),
                     app.theme().color(ColorRole::WINDOW_BG));
    TerminalView* const terminal = view.get();

    LineEditor editor(terminal->buffer());
    auto shell = std::make_unique<aegir::terminal::Shell>(
        editor, terminal->buffer(), [&app]() { app.quit(0); });
    editor.set_on_line([&shell](std::u32string const& line) {
        /* The acceptance's proof that typing reached the shell: the parsed
         * line as a serial cue, before the command runs. */
        write("  terminal: line ");
        write(aegir::trinket::utf32_to_utf8(line).c_str());
        write("\n");
        shell->run_line(line);
    });
    terminal->on_key = [&editor](KeyEvent const& event) { return editor.on_key(event); };

    window.set_content(std::move(view));
    window.set_focus(terminal);
    /* The terminal starts focused, so the acceptance's typed line reaches it
     * without a click that would race the demo's. */
    window.request_focus();
    window.show();

    app.on_started = [&]() {
        write("  terminal: ready\n");
        shell->start();
        if (log.valid()) {
            (void)log.call(aegir::log::kMethodEvent,
                           static_cast<uint64_t>(aegir::log::Event::Ready));
        }
        /* No supervision signal: this session is long-lived, like the bureau
         * (specs/workbench.md). */
    };

    return app.exec();
}
