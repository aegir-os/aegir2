/*
 * aegir-editor: the toolkit's multiline editor application.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A launcher starts this as a windowed program (specs/launch.md's kind 2): its
 * own console.gui, its own runtime, its own window. The window's content is a
 * TabGroup (specs/trinket/tabs.md) with one TextEdit page per open file, so a
 * file is a tab and the tab strip is what says which one is up. A file named
 * after the program is opened at start; the File menu opens, saves and closes
 * through the session's namespace, and it is the editor's proof on the target
 * (specs/trinket/editor.md).
 */

#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/environment.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/group.h>
#include <aegir/trinket/scrollbar.h>
#include <aegir/trinket/tab_group.h>
#include <aegir/trinket/text_edit.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>
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
constexpr int kWindowWidth = 520;
constexpr int kWindowHeight = 340;

/* A file's bytes through the session's namespace (vfs.namespace, which the
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

/* The name a tab shows: the path's last component, past a drawer's slash or a
 * volume's colon. An empty leaf -- a drawer itself -- has no name of its own. */
std::u32string tab_name(std::string const& path)
{
    std::size_t start = 0;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' || path[i] == ':') {
            start = i + 1;
        }
    }
    std::string const leaf = path.substr(start);
    return leaf.empty() ? std::u32string(U"Untitled")
                        : aegir::trinket::utf8_to_utf32(leaf);
}

} // namespace

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
        write("  editor: FAIL no console.gui\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    app.set_gui_port(gui);

    Window window(app);
    window.set_title("Ed");
    window.set_rect({kWindowX, kWindowY, kWindowWidth, kWindowHeight});
    /* Close and zoom gadgets (specs/window-manager.md), with the depth gadget a
     * decorated window carries by default. Closing the editor exits it. */
    window.set_gadgets(kGadgetClose | kGadgetZoom | kGadgetDepth);
    window.on_close_requested = [&app]() { app.quit(0); };
    /* A launcher sets AEGIR_WINDOW (specs/launch.md): the specification is this
     * program's window, and the program that owns the window parses it. */
    char const* const spec = aegir::environment::getenv("AEGIR_WINDOW");
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

    /* The window is the tabs (specs/trinket/tabs.md): one page per open file. */
    auto tabs = std::make_unique<TabGroup>();
    TabGroup* const tabs_ptr = tabs.get();

    /* One page: the editor free across the row and its scrollbar a fixed strip,
     * the same shape the demo's terminal page is (specs/trinket/scrollbar.md).
     * The page is a row so the scrollbar rides beside the text. */
    auto page = std::make_unique<Group>(Group::Orientation::HORIZONTAL, 0);
    auto edit = std::make_unique<TextEdit>();
    TextEdit* const edit_ptr = edit.get();
    auto bar = std::make_unique<Scrollbar>(Scrollbar::Orientation::VERTICAL);
    Scrollbar* const bar_ptr = bar.get();
    edit->set_font(app.default_font());
    edit->set_colors(app.theme().color(ColorRole::TEXT),
                     app.theme().color(ColorRole::WINDOW_BG));
    page->add_child(std::move(edit));
    page->add_child(std::move(bar));
    page->set_weight(bar_ptr, 0);

    /* The view drives the scrollbar and the scrollbar the view, as the demo's
     * terminal page wires them: a view change re-syncs the bar, and a scroll
     * moves the view. It re-syncs each poll so a resize that changed the rows
     * shows (specs/trinket/scrollbar.md). */
    auto sync = [edit_ptr, bar_ptr]() {
        bar_ptr->set_range(edit_ptr->row_count(), edit_ptr->visible_rows());
        bar_ptr->set_value(edit_ptr->first_row());
    };
    edit_ptr->on_change = sync;
    bar_ptr->on_scroll = [edit_ptr](int first) { edit_ptr->set_first_row(first); };

    std::u32string title = U"Untitled";
    if (argc > 1 && argv[1] != nullptr) {
        std::string const text = read_file(argv[1]);
        if (text.empty()) {
            write("  editor: cannot read ");
            write(argv[1]);
            write("\n");
        } else {
            edit_ptr->set_text(text);
            title = tab_name(argv[1]);
            write("  editor: opened ");
            write(argv[1]);
            write("\n");
        }
    }
    tabs_ptr->add_page(title, std::move(page));
    sync();
    edit_ptr->set_focused(true);

    window.set_content(std::move(tabs));
    window.show();
    window.request_focus();

    app.on_poll = sync;

    app.on_started = [&]() {
        write("  editor: ready\n");
        if (log.valid()) {
            (void)log.call(aegir::log::kMethodEvent,
                           static_cast<uint64_t>(aegir::log::Event::Ready));
        }
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
    };

    return app.exec();
}
