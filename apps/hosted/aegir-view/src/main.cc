/*
 * aegir-view: the first kind-2 customer -- a windowed text viewer.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A launcher starts this as a windowed program (specs/launch.md's kind 2): it
 * is handed its own console.gui and its own runtime, and it opens its own
 * window, so it does not share the launcher's console stream. It reads the
 * file named after it through the session's namespace and shows it in a text
 * grid -- the MultiView shape the launch spec names as the first non-terminal
 * kind-2 program. A launcher's `AEGIR_WINDOW` names its window; with none it
 * picks a default clear of the terminal's.
 */

#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/environment.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/terminal_view.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/window.h>
#include <aegir/trinket/window_spec.h>
#include <sel4/sel4.h>
#include <fcntl.h>
#include <memory>
#include <string>
#include <unistd.h>

namespace {

void write(char const *text)
{
    aegir::debug_write(text);
}

/* Clear of the terminal's window and the demo's, so a screendump's samples
 * never share. A launcher's AEGIR_WINDOW overrides it (specs/launch.md). */
constexpr int kWindowX = 420;
constexpr int kWindowY = 200;
constexpr int kWindowWidth = 420;
constexpr int kWindowHeight = 300;

/* Read a file through the session's namespace (vfs.namespace, which the
 * launcher granted), or an empty string when it cannot be read. */
std::string read_file(char const *path)
{
    int const fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return {};
    }
    std::string text;
    char chunk[512];
    ssize_t have = 0;
    while ((have = ::read(fd, chunk, sizeof(chunk))) > 0) {
        text.append(chunk, static_cast<std::size_t>(have));
    }
    ::close(fd);
    return text;
}

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
        write("  view: FAIL no console.gui\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    app.set_gui_port(gui);

    Window window(app);
    window.set_title("View");
    window.set_rect({kWindowX, kWindowY, kWindowWidth, kWindowHeight});
    /* Close and zoom gadgets (specs/window-manager.md), with the depth gadget a
     * decorated window carries by default. Closing the viewer exits it, so a
     * `Run view` whose window the user is done with does not linger. */
    window.set_gadgets(kGadgetClose | kGadgetZoom | kGadgetDepth);
    window.on_close_requested = [&app]() { app.quit(0); };
    /* A launcher sets AEGIR_WINDOW (specs/launch.md): the specification is this
     * program's window, and the program that owns the window parses it. */
    char const *const spec = aegir::environment::getenv("AEGIR_WINDOW");
    if (spec != nullptr) {
        std::string title;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        if (parse_window_spec(spec, &x, &y, &width, &height, &title)) {
            window.set_rect({x, y, width, height});
            if (!title.empty()) {
                window.set_title(title.c_str());
            }
        }
    }

    auto view = std::make_unique<TerminalView>();
    /* A grid: a fixed advance, so the theme's monospace face. */
    view->set_font(app.theme().font_monospace());
    view->set_colors(app.theme().color(ColorRole::TEXT),
                     app.theme().color(ColorRole::WINDOW_BG));
    TerminalBuffer& grid = view->buffer();

    /* The file named after the program, or a line telling the user to name
     * one. The bytes come through the session's namespace. */
    if (argc > 1 && argv[1] != nullptr) {
        std::string const text = read_file(argv[1]);
        if (text.empty()) {
            grid.write("view: cannot read ");
            grid.write(argv[1]);
            grid.write("\n");
        } else {
            grid.write(text);
            if (text.back() != '\n') {
                grid.write("\n");
            }
        }
    } else {
        grid.write("Aegir MultiView\n");
        grid.write("usage: launch view <file>\n");
    }
    grid.scroll_to_bottom();
    window.set_content(std::move(view));
    window.show();
    window.request_focus();

    app.on_started = [&]() {
        write("  view: ready\n");
        if (log.valid()) {
            (void)log.call(aegir::log::kMethodEvent,
                           static_cast<uint64_t>(aegir::log::Event::Ready));
        }
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
    };

    return app.exec();
}
